/*
 * TinyPHP.rt - pkg.c - built-in package manager core
 *
 * 设计要点（PLAN/pkg-manager.md，无 git 依赖版）：
 *  - mod.json 读写：jsonc 注释剥离 → json.c C API 解码 → 字段抽取；
 *    写出为手写 pretty printer（固定字段序）
 *  - 文件系统：libuv 同步 fs（Windows UTF-8 路径安全）
 *  - vendor/autoload.php：根项目 + vendor 下各包清单合并生成；
 *    PSR-4 按前缀长度降序（长前缀优先匹配）
 *  - PHP builtin pkg_new 走本模块同一入口（测试缝）
 *  - 阶段 B：add/remove（本地目录/zip + HTTP(S) zip；fetch 回调由 main.c 注册）
 */
#include "pkg.h"
#include "pack.h"       /* tphp_sha1_buf / tphp_sha1_file（lock integrity） */
#include "value.h"
#include "tphp.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

/* ---- 小工具 ---- */

static char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = tm_malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static void set_err(char *err, size_t cap, const char *fmt, ...) {
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* 动态字符串缓冲（tm_malloc） */
typedef struct { char *p; size_t len, cap; } PkgBuf;

static void pb_init(PkgBuf *b) { b->p = NULL; b->len = b->cap = 0; }

static void pb_reserve(PkgBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra + 1) ncap *= 2;
    b->p = tm_realloc(b->p, ncap);
    b->cap = ncap;
}

static void pb_put(PkgBuf *b, const char *s, size_t n) {
    pb_reserve(b, n);
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void pb_puts(PkgBuf *b, const char *s) { pb_put(b, s, strlen(s)); }

/* JSON 字符串字面量（含首尾引号） */
static void pb_json_str(PkgBuf *b, const char *s) {
    pb_put(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '"' || c == '\\') {
            char e[3] = { '\\', (char)c, 0 };
            pb_put(b, e, 2);
        } else if (c == '\n') pb_puts(b, "\\n");
        else if (c == '\r') pb_puts(b, "\\r");
        else if (c == '\t') pb_puts(b, "\\t");
        else if (c < 0x20) {
            char e[8];
            snprintf(e, sizeof e, "\\u%04x", c);
            pb_puts(b, e);
        } else {
            char e[2] = { (char)c, 0 };
            pb_put(b, e, 1);
        }
    }
    pb_put(b, "\"", 1);
}

/* ---- libuv 同步 fs ---- */

static bool fs_exists(const char *path) {
    uv_fs_t req;
    int r = uv_fs_stat(NULL, &req, path, NULL);
    uv_fs_req_cleanup(&req);
    return r == 0;
}

static bool fs_isdir(const char *path) {
    uv_fs_t req;
    int r = uv_fs_lstat(NULL, &req, path, NULL);
    if (r < 0) { uv_fs_req_cleanup(&req); return false; }
    bool d = S_ISDIR(req.statbuf.st_mode);
    uv_fs_req_cleanup(&req);
    return d;
}

static int fs_mkdir(const char *path) {
    if (fs_exists(path)) return 0;
    uv_fs_t req;
    int r = uv_fs_mkdir(NULL, &req, path, 0755, NULL);
    uv_fs_req_cleanup(&req);
    return (r < 0 && r != UV_EEXIST) ? r : 0;
}

static int fs_mkdir_p(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *q = buf; *q; q++) if (*q == '\\') *q = '/';
    size_t n = strlen(buf);
    while (n > 0 && buf[n - 1] == '/') buf[--n] = 0;
    if (n == 0) return UV_EINVAL;
    /* 跳过 Windows 盘符 "C:" 的冒号段 */
    char *start = buf + 1;
    if (n >= 2 && buf[1] == ':') start = buf + 2;
    for (char *p = start; *p; p++) {
        if (*p == '/') {
            *p = 0;
            int rc = fs_mkdir(buf);
            if (rc < 0) return rc;
            *p = '/';
        }
    }
    return fs_mkdir(buf);
}

static int fs_rm_rf(const char *path) {
    uv_fs_t req;
    int r = uv_fs_lstat(NULL, &req, path, NULL);
    if (r < 0) { uv_fs_req_cleanup(&req); return 0; } /* missing = ok */
    bool isdir = S_ISDIR(req.statbuf.st_mode);
    uv_fs_req_cleanup(&req);
    if (!isdir) {
        r = uv_fs_unlink(NULL, &req, path, NULL);
        uv_fs_req_cleanup(&req);
        return r < 0 ? r : 0;
    }
    uv_fs_t sr;
    r = uv_fs_scandir(NULL, &sr, path, 0, NULL);
    if (r < 0) { uv_fs_req_cleanup(&sr); return r; }
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&sr, &ent) == 0) {
        char child[1200];
        snprintf(child, sizeof child, "%s/%s", path, ent.name);
        int rc = fs_rm_rf(child);
        if (rc < 0) { uv_fs_req_cleanup(&sr); return rc; }
    }
    uv_fs_req_cleanup(&sr);
    r = uv_fs_rmdir(NULL, &req, path, NULL);
    uv_fs_req_cleanup(&req);
    return r < 0 ? r : 0;
}

static char *fs_read(const char *path, size_t *out_len) {
    uv_fs_t req;
    int r = uv_fs_open(NULL, &req, path, UV_FS_O_RDONLY, 0, NULL);
    uv_fs_req_cleanup(&req);
    if (r < 0) return NULL;
    int fd = r;
    uv_fs_t sreq;
    if (uv_fs_fstat(NULL, &sreq, fd, NULL) < 0) {
        uv_fs_req_cleanup(&sreq);
        uv_fs_close(NULL, &req, fd, NULL);
        uv_fs_req_cleanup(&req);
        return NULL;
    }
    int64_t sz = sreq.statbuf.st_size;
    uv_fs_req_cleanup(&sreq);
    if (sz < 0) sz = 0;
    char *buf = tm_malloc((size_t)sz + 1);
    size_t got = 0;
    while (got < (size_t)sz) {
        uv_buf_t iov = uv_buf_init(buf + got, (unsigned)((size_t)sz - got));
        uv_fs_t rreq;
        int n = (int)uv_fs_read(NULL, &rreq, fd, &iov, 1, (int64_t)got, NULL);
        uv_fs_req_cleanup(&rreq);
        if (n <= 0) break;
        got += (size_t)n;
    }
    buf[got] = 0;
    uv_fs_close(NULL, &req, fd, NULL);
    uv_fs_req_cleanup(&req);
    if (out_len) *out_len = got;
    return buf;
}

static int fs_write(const char *path, const char *data, size_t len) {
    uv_fs_t req;
    int r = uv_fs_open(NULL, &req, path,
                       UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC,
                       0644, NULL);
    uv_fs_req_cleanup(&req);
    if (r < 0) return r;
    int fd = r;
    uv_buf_t iov = uv_buf_init((char *)data, (unsigned)len);
    uv_fs_t wreq;
    int wr = (int)uv_fs_write(NULL, &wreq, fd, &iov, 1, 0, NULL);
    uv_fs_req_cleanup(&wreq);
    uv_fs_close(NULL, &req, fd, NULL);
    uv_fs_req_cleanup(&req);
    return wr < 0 ? wr : (wr == (int)len ? 0 : UV_EIO);
}

/* ---- jsonc 注释剥离（行注释与块注释，字符串内除外） ---- */

static char *jsonc_strip(const char *src, size_t len) {
    char *out = tm_malloc(len + 1);
    size_t n = 0, i = 0;
    while (i < len) {
        char c = src[i];
        if (c == '"') {
            out[n++] = c; i++;
            while (i < len) {
                char d = src[i];
                out[n++] = d; i++;
                if (d == '\\' && i < len) { out[n++] = src[i]; i++; continue; }
                if (d == '"') break;
            }
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = (i + 1 < len) ? i + 2 : len;
            continue;
        }
        out[n++] = c; i++;
    }
    out[n] = 0;
    return out;
}

/* ---- mod.json 清单（TMod） ---- */

typedef struct { char *prefix; char *dir; } ModPsr4;
typedef struct { char *name; char *url; char *ref; } ModDep;

/* 扩展编译契约（PLAN/ext-packages.md §2）；os 下标 windows=0 linux=1 darwin=2 */
enum { EXT_OS_WIN = 0, EXT_OS_LINUX = 1, EXT_OS_DARWIN = 2, EXT_OS_N = 3 };
typedef struct {
    char **sources; int n_sources;     /* 必填：目录（string|string[]），仅收 *.c */
    char **include; int n_include;     /* 包内 -I */
    char **defines; int n_defines;     /* -D */
    char **cflags; int n_cflags;
    char **libs; int n_libs;
    char **ldflags; int n_ldflags;
    char **os_cflags[EXT_OS_N]; int n_os_cflags[EXT_OS_N];
    char **os_libs[EXT_OS_N]; int n_os_libs[EXT_OS_N];
    char **os_ldflags[EXT_OS_N]; int n_os_ldflags[EXT_OS_N];
    char *os_cc[EXT_OS_N];
    char *tphp_abi;                    /* 与 tphp -v 比对，缺=warning 放行 */
    char *init;                        /* 链入符号；缺省约定名 */
} TExtSpec;

/* mod.json.scripts 生命周期键（PLAN/pkg-manager.md §2.1） */
enum {
    SC_PRE_INSTALL = 0, SC_POST_INSTALL,
    SC_PRE_UPDATE, SC_POST_UPDATE,
    SC_PRE_REMOVE, SC_POST_REMOVE,
    SC_N
};
static const char *const sc_keys[SC_N] = {
    "preInstall", "postInstall",
    "preUpdate", "postUpdate",
    "preRemove", "postRemove"
};

typedef struct {
    char *name, *version, *type, *author, *description, *license;
    ModPsr4 *psr4; int n_psr4;
    char **files; int n_files;
    char **pack_include; int n_include;
    char **pack_exclude; int n_exclude;
    ModDep *deps; int n_deps;
    char **scripts[SC_N]; int n_scripts[SC_N];
    ModDep *exts; int n_exts;          /* 项目侧 ext[]（§3：name/url/ref 数组） */
    TExtSpec ext_spec; bool has_ext_spec; /* 扩展包编译契约（§2，type=ext） */
} TMod;

static void tmod_init(TMod *m) { memset(m, 0, sizeof *m); }

/* char** + n 的逐元素释放（ext 契约的多槽数组共用） */
static void tm_strlist_free(char **a, int n) {
    if (!a) return;
    for (int i = 0; i < n; i++) tm_free(a[i]);
    tm_free(a);
}

static void tmod_free(TMod *m) {
    tm_free(m->name); tm_free(m->version); tm_free(m->type);
    tm_free(m->author); tm_free(m->description); tm_free(m->license);
    for (int i = 0; i < m->n_psr4; i++) {
        tm_free(m->psr4[i].prefix); tm_free(m->psr4[i].dir);
    }
    tm_free(m->psr4);
    for (int i = 0; i < m->n_files; i++) tm_free(m->files[i]);
    tm_free(m->files);
    for (int i = 0; i < m->n_include; i++) tm_free(m->pack_include[i]);
    tm_free(m->pack_include);
    for (int i = 0; i < m->n_exclude; i++) tm_free(m->pack_exclude[i]);
    tm_free(m->pack_exclude);
    for (int i = 0; i < m->n_deps; i++) {
        tm_free(m->deps[i].name); tm_free(m->deps[i].url); tm_free(m->deps[i].ref);
    }
    tm_free(m->deps);
    for (int k = 0; k < SC_N; k++) {
        for (int i = 0; i < m->n_scripts[k]; i++) tm_free(m->scripts[k][i]);
        tm_free(m->scripts[k]);
    }
    for (int i = 0; i < m->n_exts; i++) {
        tm_free(m->exts[i].name); tm_free(m->exts[i].url); tm_free(m->exts[i].ref);
    }
    tm_free(m->exts);
    {
        TExtSpec *x = &m->ext_spec;
        tm_strlist_free(x->sources, x->n_sources);
        tm_strlist_free(x->include, x->n_include);
        tm_strlist_free(x->defines, x->n_defines);
        tm_strlist_free(x->cflags, x->n_cflags);
        tm_strlist_free(x->libs, x->n_libs);
        tm_strlist_free(x->ldflags, x->n_ldflags);
        for (int i = 0; i < EXT_OS_N; i++) {
            tm_strlist_free(x->os_cflags[i], x->n_os_cflags[i]);
            tm_strlist_free(x->os_libs[i], x->n_os_libs[i]);
            tm_strlist_free(x->os_ldflags[i], x->n_os_ldflags[i]);
            tm_free(x->os_cc[i]);
        }
        tm_free(x->tphp_abi); tm_free(x->init);
    }
    memset(m, 0, sizeof *m);
}

/* 借用数组查找：返回内部值指针或 NULL（key 临时串立即释放） */
static TVal *mget(TArr *o, const char *key) {
    TVal k = TV_STR(tstr_new(key, strlen(key)));
    TVal *v = tarr_get(o, k);
    tval_release(&k);
    return v;
}

static char *mget_str(TArr *o, const char *key) {
    TVal *v = mget(o, key);
    if (!v || v->t != T_STR) return NULL;
    TStr *s = (TStr *)v->u.p;
    char *r = tm_malloc(s->len + 1);
    memcpy(r, s->c, s->len + 1);
    return r;
}

static TArr *mget_obj(TArr *o, const char *key) {
    TVal *v = mget(o, key);
    return (v && v->t == T_ARR) ? (TArr *)v->u.p : NULL;
}

static void strlist_push(char ***arr, int *n, const char *s) {
    *arr = tm_realloc(*arr, sizeof(char *) * (size_t)(*n + 1));
    (*arr)[*n] = xstrdup(s);
    (*n)++;
}

/* psr-4 值可为 string 或 string[]（Composer 兼容） */
static void parse_psr4(TMod *m, TArr *psr4) {
    tarr_sync_buckets(psr4);
    for (uint32_t i = 0; i < psr4->dused; i++) {
        if (psr4->data[i].t == T_UNDEF) continue;
        /* key：按偏移反查（与 json.c arr_key_by_off 同式） */
        TVal key = TV_NULL;
        for (uint32_t b = 0; b < psr4->bcap; b++) {
            ABucket *bk = &psr4->buckets[b];
            if (bk->key.t != T_NULL && bk->val_off == i) { key = bk->key; break; }
        }
        if (key.t != T_STR) continue;
        TStr *ks = (TStr *)key.u.p;
        TVal ev = psr4->data[i];
        if (ev.t == T_STR) {
            TStr *ds = (TStr *)ev.u.p;
            m->psr4 = tm_realloc(m->psr4, sizeof(ModPsr4) * (size_t)(m->n_psr4 + 1));
            m->psr4[m->n_psr4].prefix = tm_malloc(ks->len + 1);
            memcpy(m->psr4[m->n_psr4].prefix, ks->c, ks->len + 1);
            m->psr4[m->n_psr4].dir = tm_malloc(ds->len + 1);
            memcpy(m->psr4[m->n_psr4].dir, ds->c, ds->len + 1);
            m->n_psr4++;
        } else if (ev.t == T_ARR) {
            TArr *dirs = (TArr *)ev.u.p;
            for (uint32_t j = 0; j < dirs->dused; j++) {
                if (dirs->data[j].t != T_STR) continue;
                TStr *ds = (TStr *)dirs->data[j].u.p;
                m->psr4 = tm_realloc(m->psr4, sizeof(ModPsr4) * (size_t)(m->n_psr4 + 1));
                m->psr4[m->n_psr4].prefix = tm_malloc(ks->len + 1);
                memcpy(m->psr4[m->n_psr4].prefix, ks->c, ks->len + 1);
                m->psr4[m->n_psr4].dir = tm_malloc(ds->len + 1);
                memcpy(m->psr4[m->n_psr4].dir, ds->c, ds->len + 1);
                m->n_psr4++;
            }
        }
    }
}

static void parse_strlist(TArr *a, char ***arr, int *n) {
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_STR) continue;
        TStr *s = (TStr *)a->data[i].u.p;
        char *r = tm_malloc(s->len + 1);
        memcpy(r, s->c, s->len + 1);
        strlist_push(arr, n, r);
        tm_free(r);
    }
}

/* scripts.<key>：string | string[] → char** */
static void parse_script_slot(TArr *o, const char *key, char ***arr, int *n) {
    TVal *v = mget(o, key);
    if (!v) return;
    if (v->t == T_STR) {
        TStr *s = (TStr *)v->u.p;
        char *r = tm_malloc(s->len + 1);
        memcpy(r, s->c, s->len + 1);
        strlist_push(arr, n, r);
        tm_free(r);
    } else if (v->t == T_ARR) {
        parse_strlist((TArr *)v->u.p, arr, n);
    }
}

/* ext 解析辅助（PLAN/ext-packages.md §2/§3）：assoc 判定（存在字符串键
 * → 对象）、os 键 → 平台下标、单串压入 strlist（内部拷贝）、可选字段读入 */
static bool tarr_has_str_key(TArr *a) {
    for (uint32_t b = 0; b < a->bcap; b++)
        if (a->buckets[b].key.t == T_STR) return true;
    return false;
}
static int ext_os_idx(const char *k) {
    if (strcmp(k, "windows") == 0) return EXT_OS_WIN;
    if (strcmp(k, "linux") == 0) return EXT_OS_LINUX;
    if (strcmp(k, "darwin") == 0) return EXT_OS_DARWIN;
    return -1;
}
static void strlist_push_cstr(char ***arr, int *n, const char *s) {
    size_t l = strlen(s);
    char *r = tm_malloc(l + 1);
    memcpy(r, s, l + 1);
    strlist_push(arr, n, r);
    tm_free(r);
}
static void ext_opt_strlist(TArr *o, const char *key, char ***arr, int *n) {
    TVal *v = mget(o, key);
    if (!v) return;
    if (v->t == T_ARR) parse_strlist((TArr *)v->u.p, arr, n);
    else if (v->t == T_STR) strlist_push_cstr(arr, n, ((TStr *)v->u.p)->c);
}

/* TVal（assoc 数组）→ TMod；失败返回 false 并写 err */
static bool tmod_from_json(TVal v, TMod *m, char *err, size_t errcap) {
    if (v.t != T_ARR) {
        set_err(err, errcap, "mod.json: root must be an object");
        return false;
    }
    TArr *o = (TArr *)v.u.p;
    tmod_init(m);
    m->name = mget_str(o, "name");
    m->version = mget_str(o, "version");
    m->type = mget_str(o, "type");
    m->author = mget_str(o, "author");
    m->description = mget_str(o, "description");
    m->license = mget_str(o, "license");
    if (!m->name || !m->name[0]) {
        set_err(err, errcap, "mod.json: missing required field \"name\"");
        tmod_free(m);
        return false;
    }
    if (!m->version) m->version = xstrdup("0.0.0");
    if (!m->type) m->type = xstrdup("lib");
    TArr *al = mget_obj(o, "autoload");
    if (al) {
        TArr *p4 = mget_obj(al, "psr-4");
        if (p4) parse_psr4(m, p4);
        TArr *fl = mget_obj(al, "files");
        if (fl) parse_strlist(fl, &m->files, &m->n_files);
    }
    TArr *pk = mget_obj(o, "pack");
    if (pk) {
        TArr *inc = mget_obj(pk, "include");
        if (inc) parse_strlist(inc, &m->pack_include, &m->n_include);
        TArr *exc = mget_obj(pk, "exclude");
        if (exc) parse_strlist(exc, &m->pack_exclude, &m->n_exclude);
    }
    TArr *sc = mget_obj(o, "scripts");
    if (sc) {
        for (int k = 0; k < SC_N; k++)
            parse_script_slot(sc, sc_keys[k], &m->scripts[k], &m->n_scripts[k]);
    }
    TArr *dp = mget_obj(o, "deps");
    if (dp) {
        tarr_sync_buckets(dp);
        for (uint32_t i = 0; i < dp->dused; i++) {
            if (dp->data[i].t == T_UNDEF || dp->data[i].t != T_ARR) continue;
            TVal key = TV_NULL;
            for (uint32_t b = 0; b < dp->bcap; b++) {
                ABucket *bk = &dp->buckets[b];
                if (bk->key.t != T_NULL && bk->val_off == i) { key = bk->key; break; }
            }
            if (key.t != T_STR) continue;
            TStr *ks = (TStr *)key.u.p;
            TArr *dv = (TArr *)dp->data[i].u.p;
            char *url = mget_str(dv, "url");
            if (!url) url = mget_str(dv, "git"); /* 旧字段名过渡 */
            char *ref = mget_str(dv, "ref");
            m->deps = tm_realloc(m->deps, sizeof(ModDep) * (size_t)(m->n_deps + 1));
            m->deps[m->n_deps].name = tm_malloc(ks->len + 1);
            memcpy(m->deps[m->n_deps].name, ks->c, ks->len + 1);
            m->deps[m->n_deps].url = url;
            m->deps[m->n_deps].ref = ref;
            m->n_deps++;
        }
    }

    /* ext（§3/§2）：type=ext → 编译契约对象；否则 → 项目依赖数组
     * [{name,url,ref}]。同一键两种形状靠 type/上下文区分（用户拍板）。 */
    TVal *ev = mget(o, "ext");
    bool is_ext_pkg = m->type && strcmp(m->type, "ext") == 0;
    if (is_ext_pkg) {
        /* §2.1 禁止项：扩展不许带 PHP 包设施（尽早暴露误放） */
        if (m->n_psr4 > 0 || m->n_files > 0) {
            set_err(err, errcap, "ext package must not declare autoload");
            tmod_free(m); return false;
        }
        if (m->n_deps > 0) {
            set_err(err, errcap, "ext package must not declare deps (PHP packages)");
            tmod_free(m); return false;
        }
        if (mget(o, "scripts")) {
            set_err(err, errcap, "ext package must not declare scripts (user decision §10.3)");
            tmod_free(m); return false;
        }
        if (!ev || ev->t != T_ARR) {
            set_err(err, errcap, "ext: contract object required for type=ext");
            tmod_free(m); return false;
        }
        TArr *xo = (TArr *)ev->u.p;
        TExtSpec *x = &m->ext_spec;
        TVal *sv = mget(xo, "sources");
        if (!sv || (sv->t != T_STR && sv->t != T_ARR)) {
            set_err(err, errcap, "ext.sources required (directory or directory array)");
            tmod_free(m); return false;
        }
        if (sv->t == T_STR) strlist_push_cstr(&x->sources, &x->n_sources,
                                              ((TStr *)sv->u.p)->c);
        else parse_strlist((TArr *)sv->u.p, &x->sources, &x->n_sources);
        ext_opt_strlist(xo, "include", &x->include, &x->n_include);
        ext_opt_strlist(xo, "defines", &x->defines, &x->n_defines);
        ext_opt_strlist(xo, "cflags", &x->cflags, &x->n_cflags);
        ext_opt_strlist(xo, "libs", &x->libs, &x->n_libs);
        ext_opt_strlist(xo, "ldflags", &x->ldflags, &x->n_ldflags);
        TVal *ov = mget(xo, "os");
        if (ov && ov->t == T_ARR) {
            TArr *oa = (TArr *)ov->u.p;
            tarr_sync_buckets(oa);
            for (uint32_t b = 0; b < oa->bcap; b++) {
                ABucket *bk = &oa->buckets[b];
                if (bk->key.t != T_STR) continue;
                const char *kk = ((TStr *)bk->key.u.p)->c;
                int idx = ext_os_idx(kk);
                if (idx < 0) { tphp_warn("PHP Warning: ext.os: unknown key \"%s\" ignored\n", kk); continue; }
                TVal ovv = oa->data[bk->val_off];
                if (ovv.t != T_ARR) continue;
                TArr *oo = (TArr *)ovv.u.p;
                ext_opt_strlist(oo, "cflags", &x->os_cflags[idx], &x->n_os_cflags[idx]);
                ext_opt_strlist(oo, "libs", &x->os_libs[idx], &x->n_os_libs[idx]);
                ext_opt_strlist(oo, "ldflags", &x->os_ldflags[idx], &x->n_os_ldflags[idx]);
                TVal *cv = mget(oo, "cc");
                if (cv && cv->t == T_STR) {
                    TStr *cs = (TStr *)cv->u.p;
                    x->os_cc[idx] = tm_malloc(cs->len + 1);
                    memcpy(x->os_cc[idx], cs->c, cs->len + 1);
                }
            }
        }
        x->tphp_abi = mget_str(xo, "tphp_abi");
        x->init = mget_str(xo, "init");
        m->has_ext_spec = true;
    } else if (ev && ev->t == T_ARR) {
        /* 项目侧 ext[]（§3）：纯数组 of {name,url,ref}；非数组形状（对象）
         * 出现在非 ext 包 = 形状错（防止把契约误放项目清单） */
        TArr *ea = (TArr *)ev->u.p;
        if (tarr_has_str_key(ea)) {
            set_err(err, errcap, "ext must be an array of {name,url,ref} in non-ext package");
            tmod_free(m); return false;
        }
        for (uint32_t i = 0; i < ea->dused; i++) {
            TVal e = ea->data[i];
            if (e.t != T_ARR) continue;
            TArr *eo = (TArr *)e.u.p;
            TVal *nm = mget(eo, "name");
            if (!nm || nm->t != T_STR) {
                set_err(err, errcap, "ext[%u]: name required", i);
                tmod_free(m); return false;
            }
            TStr *ns = (TStr *)nm->u.p;
            char *url = NULL, *ref = NULL;
            TVal *uv = mget(eo, "url");
            if (uv && uv->t == T_STR) {
                TStr *us = (TStr *)uv->u.p;
                url = tm_malloc(us->len + 1);
                memcpy(url, us->c, us->len + 1);
            }
            TVal *rv = mget(eo, "ref");
            if (rv && rv->t == T_STR) {
                TStr *rs = (TStr *)rv->u.p;
                ref = tm_malloc(rs->len + 1);
                memcpy(ref, rs->c, rs->len + 1);
            }
            m->exts = tm_realloc(m->exts, sizeof(ModDep) * (size_t)(m->n_exts + 1));
            m->exts[m->n_exts].name = tm_malloc(ns->len + 1);
            memcpy(m->exts[m->n_exts].name, ns->c, ns->len + 1);
            m->exts[m->n_exts].url = url;
            m->exts[m->n_exts].ref = ref;
            m->n_exts++;
        }
    }
    return true;
}

static bool tmod_load(const char *path, TMod *m, char *err, size_t errcap) {
    size_t len = 0;
    char *raw = fs_read(path, &len);
    if (!raw) {
        set_err(err, errcap, "cannot read %s", path);
        return false;
    }
    char *stripped = jsonc_strip(raw, len);
    tm_free(raw);
    TVal v;
    bool ok = tphp_json_decode_c(stripped, strlen(stripped), &v);
    tm_free(stripped);
    if (!ok) {
        set_err(err, errcap, "%s: invalid JSON", path);
        return false;
    }
    ok = tmod_from_json(v, m, err, errcap);
    tval_release(&v);
    return ok;
}

/* 写 mod.json（pretty，固定字段序） */
static int tmod_save(const char *path, const TMod *m, char *err, size_t errcap) {
    PkgBuf b;
    pb_init(&b);
    pb_puts(&b, "{\n  \"name\": ");
    pb_json_str(&b, m->name);
    pb_puts(&b, ",\n  \"version\": ");
    pb_json_str(&b, m->version ? m->version : "0.0.0");
    pb_puts(&b, ",\n  \"type\": ");
    pb_json_str(&b, m->type ? m->type : "app");
    if (m->author && m->author[0]) {
        pb_puts(&b, ",\n  \"author\": ");
        pb_json_str(&b, m->author);
    }
    if (m->description && m->description[0]) {
        pb_puts(&b, ",\n  \"description\": ");
        pb_json_str(&b, m->description);
    }
    if (m->license && m->license[0]) {
        pb_puts(&b, ",\n  \"license\": ");
        pb_json_str(&b, m->license);
    }
    if (m->n_psr4 > 0) {
        pb_puts(&b, ",\n  \"autoload\": {\n    \"psr-4\": {\n");
        for (int i = 0; i < m->n_psr4; i++) {
            pb_puts(&b, "      ");
            pb_json_str(&b, m->psr4[i].prefix);
            pb_puts(&b, ": ");
            pb_json_str(&b, m->psr4[i].dir);
            pb_puts(&b, (i + 1 < m->n_psr4) ? ",\n" : "\n");
        }
        pb_puts(&b, "    }");
        if (m->n_files > 0) {
            pb_puts(&b, ",\n    \"files\": [\n");
            for (int i = 0; i < m->n_files; i++) {
                pb_puts(&b, "      ");
                pb_json_str(&b, m->files[i]);
                pb_puts(&b, (i + 1 < m->n_files) ? ",\n" : "\n");
            }
            pb_puts(&b, "    ]");
        }
        pb_puts(&b, "\n  }");
    } else if (m->n_files > 0) {
        pb_puts(&b, ",\n  \"autoload\": {\n    \"files\": [\n");
        for (int i = 0; i < m->n_files; i++) {
            pb_puts(&b, "      ");
            pb_json_str(&b, m->files[i]);
            pb_puts(&b, (i + 1 < m->n_files) ? ",\n" : "\n");
        }
        pb_puts(&b, "    ]\n  }");
    }
    if (m->n_include > 0 || m->n_exclude > 0) {
        pb_puts(&b, ",\n  \"pack\": {");
        if (m->n_include > 0) {
            pb_puts(&b, "\n    \"include\": [\n");
            for (int i = 0; i < m->n_include; i++) {
                pb_puts(&b, "      ");
                pb_json_str(&b, m->pack_include[i]);
                pb_puts(&b, (i + 1 < m->n_include) ? ",\n" : "\n");
            }
            pb_puts(&b, "    ]");
        }
        if (m->n_exclude > 0) {
            if (m->n_include > 0) pb_puts(&b, ",");
            pb_puts(&b, "\n    \"exclude\": [\n");
            for (int i = 0; i < m->n_exclude; i++) {
                pb_puts(&b, "      ");
                pb_json_str(&b, m->pack_exclude[i]);
                pb_puts(&b, (i + 1 < m->n_exclude) ? ",\n" : "\n");
            }
            pb_puts(&b, "    ]");
        }
        pb_puts(&b, "\n  }");
    }
    {
        bool any = false;
        for (int k = 0; k < SC_N; k++)
            if (m->n_scripts[k] > 0) { any = true; break; }
        if (any) {
            pb_puts(&b, ",\n  \"scripts\": {");
            bool firstk = true;
            for (int k = 0; k < SC_N; k++) {
                if (m->n_scripts[k] <= 0) continue;
                pb_puts(&b, firstk ? "\n" : ",\n");
                firstk = false;
                pb_puts(&b, "    ");
                pb_json_str(&b, sc_keys[k]);
                if (m->n_scripts[k] == 1) {
                    pb_puts(&b, ": ");
                    pb_json_str(&b, m->scripts[k][0]);
                } else {
                    pb_puts(&b, ": [\n");
                    for (int i = 0; i < m->n_scripts[k]; i++) {
                        pb_puts(&b, "      ");
                        pb_json_str(&b, m->scripts[k][i]);
                        pb_puts(&b, (i + 1 < m->n_scripts[k]) ? ",\n" : "\n");
                    }
                    pb_puts(&b, "    ]");
                }
            }
            pb_puts(&b, "\n  }");
        }
    }
    if (m->n_deps > 0) {
        pb_puts(&b, ",\n  \"deps\": {\n");
        for (int i = 0; i < m->n_deps; i++) {
            pb_puts(&b, "    ");
            pb_json_str(&b, m->deps[i].name);
            pb_puts(&b, ": {\n      \"url\": ");
            pb_json_str(&b, m->deps[i].url ? m->deps[i].url : "");
            if (m->deps[i].ref && m->deps[i].ref[0]) {
                pb_puts(&b, ",\n      \"ref\": ");
                pb_json_str(&b, m->deps[i].ref);
            }
            pb_puts(&b, "\n    }");
            pb_puts(&b, (i + 1 < m->n_deps) ? ",\n" : "\n");
        }
        pb_puts(&b, "  }");
    }
    /* ext：项目侧数组（§3）与扩展契约对象（§2）互斥写出（type 驱动） */
    if (m->n_exts > 0) {
        pb_puts(&b, ",\n  \"ext\": [\n");
        for (int i = 0; i < m->n_exts; i++) {
            pb_puts(&b, "    { \"name\": ");
            pb_json_str(&b, m->exts[i].name);
            pb_puts(&b, ", \"url\": ");
            pb_json_str(&b, m->exts[i].url ? m->exts[i].url : "");
            if (m->exts[i].ref && m->exts[i].ref[0]) {
                pb_puts(&b, ", \"ref\": ");
                pb_json_str(&b, m->exts[i].ref);
            }
            pb_puts(&b, " }");
            pb_puts(&b, (i + 1 < m->n_exts) ? ",\n" : "\n");
        }
        pb_puts(&b, "  ]");
    } else if (m->has_ext_spec) {
        const TExtSpec *x = &m->ext_spec;
        pb_puts(&b, ",\n  \"ext\": {\n    \"sources\": ");
        if (x->n_sources == 1) {
            pb_json_str(&b, x->sources[0]);
        } else {
            pb_puts(&b, "[\n");
            for (int i = 0; i < x->n_sources; i++) {
                pb_puts(&b, "      ");
                pb_json_str(&b, x->sources[i]);
                pb_puts(&b, (i + 1 < x->n_sources) ? ",\n" : "\n");
            }
            pb_puts(&b, "    ]");
        }
        /* 可选字符串数组字段（有则写） */
        struct { const char *k; char **v; int n; } opt[] = {
            { "include", x->include, x->n_include },
            { "defines", x->defines, x->n_defines },
            { "cflags", x->cflags, x->n_cflags },
            { "libs", x->libs, x->n_libs },
            { "ldflags", x->ldflags, x->n_ldflags },
        };
        for (size_t i = 0; i < sizeof opt / sizeof opt[0]; i++) {
            if (opt[i].n <= 0) continue;
            pb_puts(&b, ",\n    ");
            pb_puts(&b, "\"");
            pb_puts(&b, opt[i].k);
            pb_puts(&b, "\": [");
            for (int j = 0; j < opt[i].n; j++) {
                pb_puts(&b, j ? ", " : " ");
                pb_json_str(&b, opt[i].v[j]);
            }
            pb_puts(&b, opt[i].n ? " ]" : "]");
        }
        /* os 分平台（§2.2：有内容的键才写） */
        struct { const char *k; int idx; } oss[] = {
            { "windows", EXT_OS_WIN }, { "linux", EXT_OS_LINUX },
            { "darwin", EXT_OS_DARWIN },
        };
        bool os_any = false;
        for (int oi = 0; oi < EXT_OS_N; oi++)
            if (x->n_os_cflags[oi] || x->n_os_libs[oi] || x->n_os_ldflags[oi] ||
                x->os_cc[oi]) os_any = true;
        if (os_any) {
            pb_puts(&b, ",\n    \"os\": {");
            bool firsto = true;
            for (size_t oi = 0; oi < sizeof oss / sizeof oss[0]; oi++) {
                int idx = oss[oi].idx;
                if (x->n_os_cflags[idx] == 0 && x->n_os_libs[idx] == 0 &&
                    x->n_os_ldflags[idx] == 0 && !x->os_cc[idx]) continue;
                pb_puts(&b, firsto ? "\n" : ",\n");
                firsto = false;
                pb_puts(&b, "      ");
                pb_puts(&b, "\"");
                pb_puts(&b, oss[oi].k);
                pb_puts(&b, "\": {");
                struct { const char *k; char **v; int n; } oo2[] = {
                    { "cflags", x->os_cflags[idx], x->n_os_cflags[idx] },
                    { "libs", x->os_libs[idx], x->n_os_libs[idx] },
                    { "ldflags", x->os_ldflags[idx], x->n_os_ldflags[idx] },
                };
                bool firstf = true;
                for (size_t fi = 0; fi < sizeof oo2 / sizeof oo2[0]; fi++) {
                    if (oo2[fi].n <= 0) continue;
                    pb_puts(&b, firstf ? " \"" : ", \"");
                    firstf = false;
                    pb_puts(&b, oo2[fi].k);
                    pb_puts(&b, "\": [");
                    for (int j = 0; j < oo2[fi].n; j++) {
                        pb_puts(&b, j ? ", " : " ");
                        pb_json_str(&b, oo2[fi].v[j]);
                    }
                    pb_puts(&b, " ]");
                }
                if (x->os_cc[idx]) {
                    pb_puts(&b, firstf ? " \"cc\": " : ", \"cc\": ");
                    pb_json_str(&b, x->os_cc[idx]);
                }
                pb_puts(&b, " }");
            }
            pb_puts(&b, "\n    }");
        }
        if (x->tphp_abi) {
            pb_puts(&b, ",\n    \"tphp_abi\": ");
            pb_json_str(&b, x->tphp_abi);
        }
        if (x->init) {
            pb_puts(&b, ",\n    \"init\": ");
            pb_json_str(&b, x->init);
        }
        pb_puts(&b, "\n  }");
    }
    pb_puts(&b, "\n}\n");
    int rc = fs_write(path, b.p, b.len);
    tm_free(b.p);
    if (rc < 0) set_err(err, errcap, "cannot write %s", path);
    return rc < 0 ? 2 : 0;
}

/* ---- lifecycle scripts runner（PLAN/pkg-manager.md §2.1） ---- */

static char g_self_exe[1024];
static bool g_ignore_scripts = false;

void tphp_pkg_set_self_exe(const char *path) {
    if (!path || !path[0]) return;
    snprintf(g_self_exe, sizeof g_self_exe, "%s", path);
}

void tphp_pkg_set_ignore_scripts(bool on) { g_ignore_scripts = on; }

static int scripts_enabled(void) {
    if (g_ignore_scripts) return 0;
    const char *e = getenv("TPHP_PKG_SCRIPTS");
    if (e && e[0] == '0') return 0;
    return 1;
}

static int shell_scripts_enabled(void) {
    const char *e = getenv("TPHP_PKG_SCRIPTS_SHELL");
    return e && e[0] == '1';
}

static bool rel_path_ok(const char *rel) {
    if (!rel || !rel[0]) return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (strlen(rel) >= 2 && rel[1] == ':') return false;   /* C:\ */
    if (strstr(rel, "..")) return false;
    return true;
}

static bool ends_with_ci(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void pkg_env_set(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    setenv(k, v ? v : "", 1);
#endif
}

/* 同步跑子进程（cwd=pkg_dir）。 */
static int g_child_status = -1;
static void on_child_exit(uv_process_t *h, int64_t status, int term) {
    (void)h; (void)term;
    g_child_status = (int)status;
}

static int run_child(const char *exe, char *const args[], const char *cwd,
                     char *err, size_t errcap) {
    uv_loop_t *loop = uv_default_loop();
    uv_process_t proc;
    uv_process_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.file = (char *)exe;
    opts.args = args;
    opts.cwd = (char *)cwd;
    opts.exit_cb = on_child_exit;
    g_child_status = -1;
    int r = uv_spawn(loop, &proc, &opts);
    if (r != 0) {
        set_err(err, errcap, "spawn %s failed: %s", exe, uv_strerror(r));
        return 2;
    }
    uv_run(loop, UV_RUN_DEFAULT);
    if (g_child_status != 0) {
        set_err(err, errcap, "%s exited with status %d",
                exe, g_child_status);
        return 3;
    }
    return 0;
}

/* 执行一个 scripts 条目。cwd = 包根/项目根。 */
static int run_script_entry(const char *entry, const char *cwd,
                            char *err, size_t errcap) {
    if (!rel_path_ok(entry)) {
        set_err(err, errcap, "script path escapes package root: %s", entry);
        return 1;
    }
    bool is_php = ends_with_ci(entry, ".php");
    if (!is_php && !shell_scripts_enabled()) {
        set_err(err, errcap,
                "non-PHP script %s blocked (set TPHP_PKG_SCRIPTS_SHELL=1 "
                "to allow shell commands)", entry);
        return 1;
    }
    if (is_php) {
        if (!g_self_exe[0]) {
            set_err(err, errcap, "self exe not set for PHP scripts");
            return 2;
        }
        char script[1400];
        snprintf(script, sizeof script, "%s/%s", cwd, entry);
        /* 相对 entry 传给子进程 + cwd，避免绝对路径引号问题 */
        char *args[3];
        args[0] = g_self_exe;
        args[1] = (char *)entry;
        args[2] = NULL;
        /* script 存在性 */
        if (!fs_exists(script)) {
            set_err(err, errcap, "script not found: %s", script);
            return 1;
        }
        return run_child(g_self_exe, args, cwd, err, errcap);
    }
    /* shell 命令（opt-in）：经平台 shell 在 cwd 执行 */
#ifdef _WIN32
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "cd /d \"%s\" && %s", cwd, entry);
    int rc = system(cmd);
    int st = rc == -1 ? -1 : (rc & 0xFF);
#else
    char cmd[1600];
    snprintf(cmd, sizeof cmd, "cd \"%s\" && %s", cwd, entry);
    int rc = system(cmd);
    int st = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
    if (st != 0) {
        set_err(err, errcap, "command failed (%d): %s", st, entry);
        return 3;
    }
    return 0;
}

typedef struct {
    const char *action;   /* install|update|remove */
    const char *phase;    /* pre|post */
    const char *pkg_name;
    const char *pkg_version;
    const char *pkg_ref;
    const char *pkg_dir;
    const char *project_dir;
    int is_root;
} ScriptCtx;

static void fill_script_env(const ScriptCtx *c) {
    pkg_env_set("TPHP_PKG_ACTION", c->action);
    pkg_env_set("TPHP_PKG_PHASE", c->phase);
    pkg_env_set("TPHP_PKG_NAME", c->pkg_name ? c->pkg_name : "");
    pkg_env_set("TPHP_PKG_VERSION", c->pkg_version ? c->pkg_version : "");
    pkg_env_set("TPHP_PKG_REF", c->pkg_ref ? c->pkg_ref : "");
    pkg_env_set("TPHP_PKG_DIR", c->pkg_dir ? c->pkg_dir : "");
    pkg_env_set("TPHP_PROJECT_DIR", c->project_dir ? c->project_dir : "");
    pkg_env_set("TPHP_PKG_ROOT", c->is_root ? "1" : "0");
}

/* 跑 m 的 scripts[key]。pre 失败 → 返回非 0（调用方中止）；
 * post 失败 → 警告并返回 3（状态已提交，不回滚）。 */
static int run_script_slot(const TMod *m, int key, const ScriptCtx *c,
                           char *err, size_t errcap) {
    if (!scripts_enabled()) return 0;
    if (!m || m->n_scripts[key] <= 0) return 0;
    if (!c->pkg_dir || !c->pkg_dir[0]) return 0;
    fill_script_env(c);
    int worst = 0;
    for (int i = 0; i < m->n_scripts[key]; i++) {
        char e2[512] = "";
        int rc = run_script_entry(m->scripts[key][i], c->pkg_dir,
                                  e2, sizeof e2);
        if (rc == 0) continue;
        if (rc == 3) {
            tphp_warn("PHP Warning: pkg script %s (%s/%s) failed: %s\n",
                      m->scripts[key][i], c->action, sc_keys[key], e2);
            worst = 3;
            continue;
        }
        /* pre 类硬失败（1/2）：立即返回 */
        set_err(err, errcap, "%s script %s failed: %s",
                sc_keys[key], m->scripts[key][i], e2);
        return rc;
    }
    return worst;
}

/* 从 dir 的 mod.json 跑某槽（dir 可能尚无 mod.json → 0） */
static int run_scripts_from_dir(const char *dir, int key,
                                const ScriptCtx *c,
                                char *err, size_t errcap) {
    if (!scripts_enabled() || !dir || !dir[0]) return 0;
    char mj[1450];
    snprintf(mj, sizeof mj, "%s/mod.json", dir);
    if (!fs_exists(mj)) return 0;
    TMod m;
    if (!tmod_load(mj, &m, NULL, 0)) return 0;
    ScriptCtx cc = *c;
    cc.pkg_dir = dir;
    int rc = run_script_slot(&m, key, &cc, err, errcap);
    tmod_free(&m);
    return rc;
}

/* ---- 名字校验 ---- */

static bool name_seg_ok(const char *s, size_t n) {
    if (n == 0) return false;
    char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9'))) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            return false;
    }
    return true;
}

bool tphp_pkg_valid_name(const char *name, bool is_lib, const char **why) {
    if (!name || !name[0]) { if (why) *why = "name is empty"; return false; }
    const char *slash = strchr(name, '/');
    if (!is_lib) {
        if (slash) { if (why) *why = "app name must not contain \"/\""; return false; }
        if (!name_seg_ok(name, strlen(name))) {
            if (why) *why = "app name must match [a-z0-9][a-z0-9_-]*";
            return false;
        }
        return true;
    }
    if (!slash) {
        if (why) *why = "lib name must be \"vendor/package\"";
        return false;
    }
    if (strchr(slash + 1, '/')) {
        if (why) *why = "lib name must contain exactly one \"/\"";
        return false;
    }
    if (!name_seg_ok(name, (size_t)(slash - name))) {
        if (why) *why = "lib vendor part must match [a-z0-9][a-z0-9_-]*";
        return false;
    }
    if (!name_seg_ok(slash + 1, strlen(slash + 1))) {
        if (why) *why = "lib package part must match [a-z0-9][a-z0-9_-]*";
        return false;
    }
    return true;
}

/* foo/bar → Foo\Bar\（段首大写，- 换成 _） */
static char *ns_prefix_from_name(const char *name) {
    PkgBuf b;
    pb_init(&b);
    bool seg_start = true;
    for (const char *p = name; *p; p++) {
        if (*p == '/') { pb_puts(&b, "\\"); seg_start = true; continue; }
        char c = *p;
        if (c == '-') c = '_';
        else if (seg_start && c >= 'a' && c <= 'z') c = (char)(c - 32);
        seg_start = false;
        pb_put(&b, &c, 1);
    }
    pb_puts(&b, "\\");
    return b.p;
}

/* ---- vendor/autoload.php 生成 ---- */

/* PHP 双引号串字面量（生成代码用；路径/前缀已知无 $ 之外的插值风险时仍全量转义） */
static void pb_php_dq(PkgBuf *b, const char *s) {
    pb_put(b, "\"", 1);
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"') pb_put(b, "\\", 1);
        else if (*p == '$') pb_put(b, "\\", 1);
        pb_put(b, p, 1);
    }
    pb_put(b, "\"", 1);
}

/* 确保 PSR-4 前缀以 \ 结尾 */
static char *psr4_prefix_norm(const char *prefix) {
    size_t n = strlen(prefix);
    bool need = n == 0 || prefix[n - 1] != '\\';
    char *r = tm_malloc(n + 2);
    memcpy(r, prefix, n);
    if (need) { r[n] = '\\'; r[n + 1] = 0; }
    else r[n] = 0;
    return r;
}

/* 路径分隔符归一为 /，去尾部 / */
static char *path_fwd(char *s) {
    for (char *q = s; *q; q++) if (*q == '\\') *q = '/';
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == '/') s[--n] = 0;
    return s;
}

/* 前缀长度降序（长前缀优先；稳定：等长保持原序） */
static void sort_psr4_len_desc(ModPsr4 *a, int n) {
    for (int i = 1; i < n; i++) {
        ModPsr4 key = a[i];
        int j = i - 1;
        while (j >= 0 && strlen(a[j].prefix) < strlen(key.prefix)) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

typedef struct { char **names; int n; } StrList;

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* 收集 vendor 下全部含 mod.json 的包目录（两级：vendor/<a>/<b>），排序返回 */
static StrList collect_vendor_pkgs(const char *vendor_dir) {
    StrList sl = { NULL, 0 };
    uv_fs_t req;
    int r = uv_fs_scandir(NULL, &req, vendor_dir, 0, NULL);
    if (r < 0) { uv_fs_req_cleanup(&req); return sl; }
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;
        char sub[1200];
        snprintf(sub, sizeof sub, "%s/%s", vendor_dir, ent.name);
        if (!fs_isdir(sub)) continue;
        uv_fs_t r2;
        int rsc = uv_fs_scandir(NULL, &r2, sub, 0, NULL);
        if (rsc < 0) { uv_fs_req_cleanup(&r2); continue; }
        uv_dirent_t e2;
        while (uv_fs_scandir_next(&r2, &e2) == 0) {
            if (e2.name[0] == '.') continue;
            char pkg[1400];
            snprintf(pkg, sizeof pkg, "%s/%s", sub, e2.name);
            char mj[1450];
            snprintf(mj, sizeof mj, "%s/mod.json", pkg);
            if (!fs_isdir(pkg) || !fs_exists(mj)) continue;
            /* 名字带 vendor 段以区分同名 —— 存相对路径 "a/b" */
            sl.names = tm_realloc(sl.names, sizeof(char *) * (size_t)(sl.n + 1));
            PkgBuf full;
            pb_init(&full);
            pb_puts(&full, ent.name);
            pb_puts(&full, "/");
            pb_puts(&full, e2.name);
            sl.names[sl.n] = full.p;
            sl.n++;
        }
        uv_fs_req_cleanup(&r2);
    }
    uv_fs_req_cleanup(&req);
    if (sl.n > 1) qsort(sl.names, (size_t)sl.n, sizeof(char *), cmp_str);
    return sl;
}

int tphp_pkg_gen_autoload(const char *proj_dir, char *err, size_t errcap) {
    char vendor_dir[1200], out_path[1250], root_mj[1250];
    snprintf(vendor_dir, sizeof vendor_dir, "%s/vendor", proj_dir);
    snprintf(out_path, sizeof out_path, "%s/vendor/autoload.php", proj_dir);
    snprintf(root_mj, sizeof root_mj, "%s/mod.json", proj_dir);

    /* 根清单（可缺省：仅 vendor 依赖时） */
    TMod root;
    bool has_root = false;
    if (fs_exists(root_mj)) {
        if (!tmod_load(root_mj, &root, err, errcap)) return 2;
        has_root = true;
    } else {
        tmod_init(&root);
    }

    /* 收集依赖包清单 */
    StrList pkgs = collect_vendor_pkgs(vendor_dir);
    TMod *mods = tm_malloc(sizeof(TMod) * (size_t)(pkgs.n > 0 ? pkgs.n : 1));
    int nmod = 0;
    for (int i = 0; i < pkgs.n; i++) {
        char mjp[1400];
        snprintf(mjp, sizeof mjp, "%s/%s/mod.json", vendor_dir, pkgs.names[i]);
        TMod m;
        if (tmod_load(mjp, &m, err, errcap)) {
            mods[nmod++] = m;
        } else {
            /* 依赖清单坏了不致命：警告并跳过 */
            fprintf(stderr, "tphp: warning: %s\n", err);
            if (err) err[0] = 0;
        }
    }

    /* 组装生成内容 */
    PkgBuf b;
    pb_init(&b);
    pb_puts(&b, "<?php\n");
    pb_puts(&b, "/* AUTO-GENERATED by tphp — do not edit.\n");
    pb_puts(&b, " * Recomputed on every install/add/remove. */\n\n");

    /* 1) files */
    bool any_files = false;
    if (has_root) {
        for (int i = 0; i < root.n_files; i++) {
            char *fwd = path_fwd(root.files[i]);
            pb_puts(&b, "require_once __DIR__ . \"/../");
            pb_puts(&b, fwd);
            pb_puts(&b, "\";\n");
            any_files = true;
        }
    }
    for (int mi = 0; mi < nmod; mi++) {
        for (int i = 0; i < mods[mi].n_files; i++) {
            char *fwd = path_fwd(mods[mi].files[i]);
            pb_puts(&b, "require_once __DIR__ . \"/");
            pb_puts(&b, pkgs.names[mi]);
            pb_puts(&b, "/");
            pb_puts(&b, fwd);
            pb_puts(&b, "\";\n");
            any_files = true;
        }
    }
    if (any_files) pb_puts(&b, "\n");

    /* 2) PSR-4 表（root 在前，其后按包名排序；整体长前缀优先） */
    /* 展开为扁平数组便于统一排序 */
    ModPsr4 *all = NULL;
    int nall = 0;
    if (has_root) {
        for (int i = 0; i < root.n_psr4; i++) {
            char *pfx = psr4_prefix_norm(root.psr4[i].prefix);
            char *dir = xstrdup(root.psr4[i].dir);
            path_fwd(dir);
            /* 根路径：vendor/ 的上一级 */
            PkgBuf rel;
            pb_init(&rel);
            pb_puts(&rel, "/../");
            pb_puts(&rel, dir);
            tm_free(dir);
            all = tm_realloc(all, sizeof(ModPsr4) * (size_t)(nall + 1));
            all[nall].prefix = pfx;
            all[nall].dir = rel.p;
            nall++;
        }
    }
    for (int mi = 0; mi < nmod; mi++) {
        for (int i = 0; i < mods[mi].n_psr4; i++) {
            char *pfx = psr4_prefix_norm(mods[mi].psr4[i].prefix);
            char *dir = xstrdup(mods[mi].psr4[i].dir);
            path_fwd(dir);
            PkgBuf rel;
            pb_init(&rel);
            pb_puts(&rel, "/");
            pb_puts(&rel, pkgs.names[mi]);
            pb_puts(&rel, "/");
            pb_puts(&rel, dir);
            tm_free(dir);
            all = tm_realloc(all, sizeof(ModPsr4) * (size_t)(nall + 1));
            all[nall].prefix = pfx;
            all[nall].dir = rel.p;
            nall++;
        }
    }
    /* 稳定长前缀优先：先按包序（root→deps），再整体按前缀长度降序 */
    sort_psr4_len_desc(all, nall);

    pb_puts(&b, "$psr4 = [\n");
    for (int i = 0; i < nall; i++) {
        pb_puts(&b, "    ");
        pb_php_dq(&b, all[i].prefix);
        pb_puts(&b, " => [__DIR__ . ");
        pb_php_dq(&b, all[i].dir);
        pb_puts(&b, "],\n");
    }
    pb_puts(&b, "];\n\n");

    /* 3) autoloader（不用 strncmp——stdlib 无此函数；substr+strlen 等价前缀匹配） */
    pb_puts(&b,
        "spl_autoload_register(function (string $class) use ($psr4) {\n"
        "    $norm = str_replace(\"\\\\\", \"/\", $class) . \".php\";\n"
        "    foreach ($psr4 as $prefix => $dirs) {\n"
        "        if (substr($class, 0, strlen($prefix)) !== $prefix) continue;\n"
        "        $rel = substr($norm, strlen(str_replace(\"\\\\\", \"/\", $prefix)));\n"
        "        foreach ($dirs as $dir) {\n"
        "            $f = $dir . \"/\" . $rel;\n"
        "            if (file_exists($f)) { require $f; return; }\n"
        "        }\n"
        "    }\n"
        "});\n");

    if (fs_mkdir_p(vendor_dir) < 0) {
        set_err(err, errcap, "cannot create %s", vendor_dir);
        goto fail;
    }
    if (fs_write(out_path, b.p, b.len) < 0) {
        set_err(err, errcap, "cannot write %s", out_path);
        goto fail;
    }
    for (int i = 0; i < nall; i++) { tm_free(all[i].prefix); tm_free(all[i].dir); }
    tm_free(all);
    for (int i = 0; i < nmod; i++) tmod_free(&mods[i]);
    tm_free(mods);
    for (int i = 0; i < pkgs.n; i++) tm_free(pkgs.names[i]);
    tm_free(pkgs.names);
    if (has_root) tmod_free(&root);
    tm_free(b.p);
    return 0;

fail:
    for (int i = 0; i < nall; i++) { tm_free(all[i].prefix); tm_free(all[i].dir); }
    tm_free(all);
    for (int i = 0; i < nmod; i++) tmod_free(&mods[i]);
    tm_free(mods);
    for (int i = 0; i < pkgs.n; i++) tm_free(pkgs.names[i]);
    tm_free(pkgs.names);
    if (has_root) tmod_free(&root);
    tm_free(b.p);
    return 2;
}

/* ---- 骨架文件内容 ---- */

static const char APP_MAIN_PHP[] =
    "<?php\n"
    "require \"vendor/autoload.php\";\n"
    "\n"
    "echo \"Hello, world\\n\";\n";

static const char LIB_GREETER_TMPL[] =
    "<?php\n"
    "namespace %s;\n"
    "\n"
    "class Greeter\n"
    "{\n"
    "    public static function hello(): string\n"
    "    {\n"
    "        return \"hello from %s\";\n"
    "    }\n"
    "}\n";

static const char LIB_SMOKE_TMPL[] =
    "<?php\n"
    "// EXPECT: smoke=hello from %s\n"
    "require __DIR__ . \"/../vendor/autoload.php\";\n"
    "\n"
    "use %s\\Greeter;\n"
    "\n"
    "echo \"smoke=\", Greeter::hello(), \"\\n\";\n";

/* ---- tphp new ---- */

/* ---- ext C 扩展脚手架（PLAN/ext-packages.md §2.3） ---- */

bool tphp_pkg_ext_root(char *buf, size_t cap) {
    if (!g_self_exe[0]) return false;
    snprintf(buf, cap, "%s", g_self_exe);
    char *sep = NULL;
    for (char *p = buf; *p; p++)
        if (*p == '/' || *p == '\\') sep = p;
    if (!sep) { buf[0] = 0; return false; }   /* argv0 无路径：无法定位 */
    *sep = 0;
    return buf[0] != 0;
}

/* 约定 init 符号：name 中 / 与 - → _（§2.3）→ tphp_ext_<vendor>_<pkg>_init */
static void ext_init_sym(const char *name, char *out, size_t cap) {
    size_t n = 0;
    const char *pre = "tphp_ext_";
    for (const char *p = pre; *p && n + 1 < cap; p++) out[n++] = *p;
    for (const char *p = name; *p && n + 1 < cap; p++) {
        char c = *p;
        if (c == '/' || c == '-' || c == '.') c = '_';
        out[n++] = c;
    }
    const char *suf = "_init";
    for (const char *p = suf; *p && n + 1 < cap; p++) out[n++] = *p;
    out[n] = 0;
}

/* ext 脚手架模板（PLAN/scaffold.md §4.1，2026-11-12 拍板：质量对齐 app/lib
 * + C 桩自注册最小三件）。EXT_C_TMPL 的 %s 顺序（9 个，勿乱）：
 * name, ns_php, name, ns_php, ns_c, ns_c, sym, ns_c, ns_c。
 * ns_php = acme\hello（PHP 注释/声明形）；ns_c = acme\\hello（C 字面量形）。 */
static const char EXT_C_TMPL[] =
    "/* %s - tphp C extension (scaffold; PLAN/ext-packages.md 2.3 + §2.4)。\n"
    " * 自注册最小三件（2026-11-12 拍板）：常量 VERSION + 函数 hello + 空类 Example——\n"
    " * `tphp ext --add <本包>` 链入后 examples/usage.php 开箱可跑。\n"
    " * 动手改注册内容前先读 PLAN/ext-packages.md §2.4（注册 API v1）。 */\n"
    "#include \"pkg.h\"      /* tphp_ext_reg_const_str / tphp_ext_reg_fn */\n"
    "#include \"class.h\"    /* TClass / tphp_class_register_compile */\n"
    "#include \"value.h\"    /* TVal / TV_* / tstr_* / tm_malloc / CK_CLASS */\n"
    "#include \"opcode.h\"   /* VM / BuiltinFn */\n"
    "#include <stdio.h>\n"
    "#include <string.h>\n"
    "\n"
    "/* 示例函数：%s\\hello(string $who) —— 手写判型（最小桩不引 builtin.h） */\n"
    "static TVal scaffold_hello(VM *vm, TVal *args, int n) {\n"
    "    (void)vm;\n"
    "    const char *who = (n >= 1 && args[0].t == T_STR)\n"
    "                    ? tstr_val((TStr *)args[0].u.p) : \"world\";\n"
    "    char buf[256];\n"
    "    snprintf(buf, sizeof buf, \"hello, %%s (from %s)\", who);\n"
    "    return TV_STR(tstr_new(buf, strlen(buf)));\n"
    "}\n"
    "\n"
    "/* 示例空类：%s\\Example（无成员；exclass 蓝本最小子集） */\n"
    "static TClass *scaffold_example_class(void) {\n"
    "    TClass *cls = tm_malloc(sizeof *cls);\n"
    "    memset(cls, 0, sizeof *cls);\n"
    "    cls->name = tstr_intern(\"%s\\\\Example\", strlen(\"%s\\\\Example\"));\n"
    "    cls->kind = CK_CLASS;\n"
    "    cls->m_ctor = cls->m_dtor = cls->m_get = cls->m_set = cls->m_isset =\n"
    "        cls->m_unset = cls->m_call = cls->m_callstatic = cls->m_tostring =\n"
    "        cls->m_invoke = cls->m_clone = cls->m_debuginfo = cls->m_clonestate = -1;\n"
    "    return cls;\n"
    "}\n"
    "\n"
    "/* 链入后 registry 调用（mod.json ext.init；首脚本编译前一次） */\n"
    "void %s(void) {\n"
    "    tphp_ext_reg_const_str(\"%s\\\\VERSION\", \"0.1.0\");\n"
    "    tphp_ext_reg_fn(\"%s\\\\hello\", scaffold_hello);\n"
    "    tphp_class_register_compile(scaffold_example_class());\n"
    "}\n";

/* 消费示例（examples/usage.php）：%s = name, ns_php */
static const char EXT_USAGE_TMPL[] =
    "<?php\n"
    "// %s 的消费示例：本包 C 扩展自注册的最小三件（src/ 模板生成）。\n"
    "// 先链入：tphp ext --add <本包路径>（重链后下次运行生效）。\n"
    "// 再跑：  tphp run examples/usage.php\n"
    "namespace %s;\n"
    "\n"
    "echo \"const: \", VERSION, \"\\n\";\n"
    "echo \"fn: \", hello(\"world\"), \"\\n\";\n"
    "$e = new Example();\n"
    "echo \"class: \", ($e instanceof Example) ? \"Example ok\" : \"?\", \"\\n\";\n";

/* README：%s = name, ns_php, ns_php, ns_php, name */
static const char EXT_README_TMPL[] =
    "# %s\n\n"
    "`tphp` C extension package. The scaffold self-registers a minimal trio via the\n"
    "registration API (PLAN/ext-packages.md §2.4): const `%s\\VERSION`, fn `%s\\hello()`,\n"
    "class `%s\\Example`.\n\n"
    "## Next\n\n"
    "```bash\n"
    "tphp ext --add <this-package-path>   # compile & relink into current tphp (next run picks it up)\n"
    "tphp run examples/usage.php         # consume the registered const/fn/class\n"
    "tphp ext --remove %s                # uninstall & relink the base tphp\n"
    "```\n\n"
    "Spec: PLAN/ext-packages.md §2.3 (contract) / §2.4 (registration API).\n";

/* ext 编译链产物（包内 build/、重链中间件） */
static const char EXT_GITIGNORE[] =
    "build/\n"
    "*.o\n"
    "*.obj\n"
    "tphp.next\n"
    "tphp.old\n";

static const char EXT_H_TMPL[] =
    "#ifndef %s\n"
    "#define %s\n"
    "\n"
    "/* %s entry (scaffold). */\n"
    "void %s(void);\n"
    "\n"
    "#endif\n";

int tphp_pkg_new_ext(const char *dir, const char *name, bool force, char *err, size_t errcap) {
    const char *why = NULL;
    if (!tphp_pkg_valid_name(name, true, &why)) {
        set_err(err, errcap, "invalid extension name \"%s\": %s (need vendor/package)",
                name ? name : "", why ? why : "");
        return 1;
    }
    if (!dir || !dir[0]) {
        set_err(err, errcap, "target directory is empty");
        return 1;
    }
    if (fs_exists(dir)) {
        if (!force) {
            set_err(err, errcap,
                    "target directory already exists: %s (use --force to overwrite scaffold)",
                    dir);
            return 2;
        }
        fs_rm_rf(dir);
    }
    if (fs_mkdir_p(dir) < 0) {
        set_err(err, errcap, "cannot create directory: %s", dir);
        return 2;
    }

    char sym[600];
    ext_init_sym(name, sym, sizeof sym);

    /* mod.json 编译契约（§2：无 scripts/autoload/deps） */
    TMod m;
    tmod_init(&m);
    m.name = xstrdup(name);
    m.version = xstrdup("0.1.0");
    m.type = xstrdup("ext");
    m.license = xstrdup("MIT");
    strlist_push_cstr(&m.ext_spec.sources, &m.ext_spec.n_sources, "src");
    strlist_push_cstr(&m.ext_spec.include, &m.ext_spec.n_include, "include/");
    m.ext_spec.tphp_abi = xstrdup(TPHP_VERSION);
    m.ext_spec.init = xstrdup(sym);
    m.has_ext_spec = true;
    char mj[1400];
    snprintf(mj, sizeof mj, "%s/mod.json", dir);
    int rc = tmod_save(mj, &m, err, errcap);
    tmod_free(&m);
    if (rc != 0) { fs_rm_rf(dir); return rc; }

    /* src/<pkg>.c + include/<pkg>.h（pkg = name 最后一段） */
    const char *slash = strrchr(name, '/');
    const char *pkg = slash ? slash + 1 : name;
    /* 派生 ns：ns_php = acme\hello（PHP 形）/ ns_c = acme\\hello（C 字面量形） */
    char ns_php[640], ns_c[640];
    size_t ni = 0, jc = 0, kc = 0;
    while (name[ni]) {
        if (name[ni] == '/') {
            if (jc + 1 < sizeof ns_php) ns_php[jc++] = '\\';
            if (kc + 2 < sizeof ns_c) { ns_c[kc++] = '\\'; ns_c[kc++] = '\\'; }
        } else {
            if (jc + 1 < sizeof ns_php) ns_php[jc++] = name[ni];
            if (kc + 1 < sizeof ns_c) ns_c[kc++] = name[ni];
        }
        ni++;
    }
    ns_php[jc] = 0;
    ns_c[kc] = 0;
    char sub[1400], body[4096];
    snprintf(sub, sizeof sub, "%s/src", dir);
    if (fs_mkdir_p(sub) < 0) {
        set_err(err, errcap, "cannot create %s", sub);
        fs_rm_rf(dir);
        return 2;
    }
    snprintf(sub, sizeof sub, "%s/include", dir);
    if (fs_mkdir_p(sub) < 0) {
        set_err(err, errcap, "cannot create %s", sub);
        fs_rm_rf(dir);
        return 2;
    }
    /* %s 序：name, ns_php, name, ns_php, ns_c, ns_c, sym, ns_c, ns_c（见模板头注） */
    snprintf(body, sizeof body, EXT_C_TMPL,
             name, ns_php, name, ns_php, ns_c, ns_c, sym, ns_c, ns_c);
    snprintf(sub, sizeof sub, "%s/src/%s.c", dir, pkg);
    if (fs_write(sub, body, strlen(body)) < 0) {
        set_err(err, errcap, "cannot write %s", sub);
        fs_rm_rf(dir);
        return 2;
    }
    char guard[700];
    snprintf(guard, sizeof guard, "TPHP_EXT_%s_INCLUDED", sym);
    snprintf(body, sizeof body, EXT_H_TMPL, guard, guard, name, sym);
    snprintf(sub, sizeof sub, "%s/include/%s.h", dir, pkg);
    if (fs_write(sub, body, strlen(body)) < 0) {
        set_err(err, errcap, "cannot write %s", sub);
        fs_rm_rf(dir);
        return 2;
    }

    /* README + .gitignore + examples/usage.php（PLAN/scaffold.md §4.1：
     * 质量对齐 app/lib；usage 配套自注册三件，链入即开箱可跑） */
    char readme[2400];
    snprintf(readme, sizeof readme, EXT_README_TMPL,
             name, ns_php, ns_php, ns_php, name);
    snprintf(sub, sizeof sub, "%s/README.md", dir);
    if (fs_write(sub, readme, strlen(readme)) < 0) {
        set_err(err, errcap, "cannot write %s", sub);
        fs_rm_rf(dir);
        return 2;
    }
    snprintf(sub, sizeof sub, "%s/.gitignore", dir);
    if (fs_write(sub, EXT_GITIGNORE, sizeof EXT_GITIGNORE - 1) < 0) {
        set_err(err, errcap, "cannot write %s", sub);
        fs_rm_rf(dir);
        return 2;
    }
    snprintf(sub, sizeof sub, "%s/examples", dir);
    if (fs_mkdir_p(sub) < 0) {
        set_err(err, errcap, "cannot create %s", sub);
        fs_rm_rf(dir);
        return 2;
    }
    snprintf(body, sizeof body, EXT_USAGE_TMPL, name, ns_php);
    snprintf(sub, sizeof sub, "%s/examples/usage.php", dir);
    if (fs_write(sub, body, strlen(body)) < 0) {
        set_err(err, errcap, "cannot write %s", sub);
        fs_rm_rf(dir);
        return 2;
    }
    return 0;
}

int tphp_pkg_new(const char *dir, const char *name, bool is_lib,
                 char *err, size_t errcap) {
    const char *why = NULL;
    if (!tphp_pkg_valid_name(name, is_lib, &why)) {
        set_err(err, errcap, "invalid package name \"%s\": %s",
                name ? name : "", why ? why : "");
        return 1;
    }
    if (!dir || !dir[0]) {
        set_err(err, errcap, "target directory is empty");
        return 1;
    }
    if (fs_exists(dir)) {
        set_err(err, errcap, "target directory already exists: %s", dir);
        return 2;
    }
    if (fs_mkdir_p(dir) < 0) {
        set_err(err, errcap, "cannot create directory: %s", dir);
        return 2;
    }

    TMod m;
    tmod_init(&m);
    m.name = xstrdup(name);
    m.version = xstrdup("0.1.0");
    m.type = xstrdup(is_lib ? "lib" : "app");
    m.license = xstrdup("MIT");
    if (is_lib) {
        m.n_psr4 = 1;
        m.psr4 = tm_malloc(sizeof(ModPsr4));
        m.psr4[0].prefix = ns_prefix_from_name(name);
        m.psr4[0].dir = xstrdup("src/");
    }

    char path[1300];
    int rc;
    snprintf(path, sizeof path, "%s/mod.json", dir);
    rc = tmod_save(path, &m, err, errcap);
    tmod_free(&m);
    if (rc != 0) { fs_rm_rf(dir); return rc; }

    if (is_lib) {
        char ns[256];
        char *nsp = ns_prefix_from_name(name);
        /* ns_prefix_from_name 末尾带 \；PHP namespace 声明不带尾 \ */
        size_t nl = strlen(nsp);
        if (nl > 0 && nsp[nl - 1] == '\\') nsp[nl - 1] = 0;
        snprintf(ns, sizeof ns, "%s", nsp);
        tm_free(nsp);

        snprintf(path, sizeof path, "%s/src", dir);
        if (fs_mkdir_p(path) < 0) goto io_fail;
        char body[1024];
        snprintf(body, sizeof body, LIB_GREETER_TMPL, ns, ns);
        snprintf(path, sizeof path, "%s/src/Greeter.php", dir);
        if (fs_write(path, body, strlen(body)) < 0) goto io_fail;

        snprintf(path, sizeof path, "%s/tests", dir);
        if (fs_mkdir_p(path) < 0) goto io_fail;
        char smoke[1024];
        snprintf(smoke, sizeof smoke, LIB_SMOKE_TMPL, ns, ns);
        snprintf(path, sizeof path, "%s/tests/test-smoke.php", dir);
        if (fs_write(path, smoke, strlen(smoke)) < 0) goto io_fail;
    } else {
        snprintf(path, sizeof path, "%s/main.php", dir);
        if (fs_write(path, APP_MAIN_PHP, sizeof APP_MAIN_PHP - 1) < 0)
            goto io_fail;
    }

    /* README.md */
    {
        PkgBuf rb;
        pb_init(&rb);
        pb_puts(&rb, "# ");
        pb_puts(&rb, name);
        pb_puts(&rb, is_lib ? "\n\n`tphp` library package.\n\n```bash\ntphp test tests\n```\n"
                            : "\n\n`tphp` application.\n\n```bash\ntphp run main.php\n```\n");
        snprintf(path, sizeof path, "%s/README.md", dir);
        if (fs_write(path, rb.p, rb.len) < 0) { tm_free(rb.p); goto io_fail; }
        tm_free(rb.p);
    }

    /* .gitignore（vendor 为生成物） */
    {
        static const char gi[] = "vendor/\n";
        snprintf(path, sizeof path, "%s/.gitignore", dir);
        if (fs_write(path, gi, sizeof gi - 1) < 0) goto io_fail;
    }

    /* vendor/autoload.php（骨架即用：根项目自身 psr-4/files） */
    if (tphp_pkg_gen_autoload(dir, err, errcap) != 0) {
        fs_rm_rf(dir);
        return 2;
    }
    return 0;

io_fail:
    set_err(err, errcap, "IO failure while scaffolding %s", dir);
    fs_rm_rf(dir);
    return 2;
}

/* ================================================================
 * 阶段 B：add / remove（HTTP zip URL + 本地路径；无 git）
 * ================================================================ */

static TphpPkgFetchFn g_fetch;
void tphp_pkg_set_fetch(TphpPkgFetchFn fn) { g_fetch = fn; }

static bool is_http_url(const char *s) {
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0;
}

/* GitHub 仓库页 URL 解析（含 `:ref` 后缀语法）：
 *   https://github.com/<owner>/<repo>[.git][/tree/<ref>][:ref]
 *   ref 优先级：CLI 参数 > URL `:ref` > /tree/<ref> > 最新发布版解析
 *   无任何 ref → releases/latest → tags semver 排序 → 默认分支回退
 * mod.json 存剥掉 `:ref` 的仓库 URL + ref 字段（解析结果固化，可复现）。 */

typedef struct GhRepo {
    char owner[128], repo[128], treeref[128];
} GhRepo;

/* 剥离 URL 末段内嵌的 `:ref` 后缀：`:` 必须位于 scheme `//` 之后 path 内
 * 最后一个 '/' 之后；ref 只允许 [A-Za-z0-9._-]（含 '/' 的 ref 请用 CLI
 * 第二参数）。带 query/fragment 的 URL 不做剥离（`:` 可能属于 query）。 */
static void gh_split_url_ref(const char *source, char *url, size_t urlcap,
                             char *ref, size_t refcap) {
    snprintf(url, urlcap, "%s", source);
    ref[0] = 0;
    if (strpbrk(source, "?#")) return;
    const char *sch = strstr(source, "//");
    if (!sch) return;
    const char *p = sch + 2;
    const char *slash = strrchr(p, '/');
    const char *colon = strrchr(p, ':');
    if (!colon || (slash && colon < slash)) return;
    const char *r = colon + 1;
    size_t n = strlen(r);
    if (n == 0 || n >= refcap || strchr(r, '/')) return;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)r[i]) && r[i] != '-' && r[i] != '_' &&
            r[i] != '.')
            return;
    size_t keep = (size_t)(colon - source);
    if (keep >= urlcap) return;
    memcpy(url, source, keep);
    url[keep] = 0;
    memcpy(ref, r, n + 1);
}

/* 解析（剥离后的）仓库页 URL → GhRepo。不匹配（其它主机/更深路径/
 * 已是 zip 直链）返回 false。 */
static bool gh_parse_repo(const char *source, GhRepo *g) {
    g->owner[0] = g->repo[0] = g->treeref[0] = 0;
    const char *p = source;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;
    else return false;
    size_t hlen = strcspn(p, "/?#");
    if (hlen != 10 || strncmp(p, "github.com", 10) != 0) return false;
    p += hlen;
    while (*p == '/') p++;

    size_t n = strcspn(p, "/?#");
    if (n == 0 || n >= sizeof g->owner) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)p[i]) && p[i] != '-' && p[i] != '_' &&
            p[i] != '.')
            return false;
    memcpy(g->owner, p, n);
    g->owner[n] = 0;
    p += n;
    if (*p == '/') p++;

    n = strcspn(p, "/?#");
    if (n == 0 || n >= sizeof g->repo) return false;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)p[i]) && p[i] != '-' && p[i] != '_' &&
            p[i] != '.')
            return false;
    memcpy(g->repo, p, n);
    g->repo[n] = 0;
    p += n;
    size_t rn = strlen(g->repo);
    if (rn > 4 && strcmp(g->repo + rn - 4, ".git") == 0)
        g->repo[rn - 4] = 0;
    if (!g->repo[0]) return false;

    /* 剩余路径：允许空或 /tree/<ref>（单段）；其它一律不算仓库页 */
    if (*p == '/') {
        p++;
        if (strncmp(p, "tree/", 5) != 0) return false;
        p += 5;
        n = strcspn(p, "/?#");
        if (n == 0 || n >= sizeof g->treeref) return false;
        memcpy(g->treeref, p, n);
        g->treeref[n] = 0;
        p += n;
        if (*p && *p != '?' && *p != '#') return false;   /* /tree/x/deeper */
    }
    return true;
}

/* 识别 GitHub 仓库页 URL（含 :ref 剥离）。匹配 → true 且写出规范化
 * url/ref（ref 可能为空）；不匹配 → url_out=source、ref_out 空。 */
static bool gh_classify(const char *source, char *url_out, size_t urlcap,
                        char *ref_out, size_t refcap, GhRepo *g) {
    char url[1400], ref[128];
    gh_split_url_ref(source, url, sizeof url, ref, sizeof ref);
    if (gh_parse_repo(url, g)) {
        snprintf(url_out, urlcap, "%s", url);
        snprintf(ref_out, refcap, "%s", ref);
        return true;
    }
    snprintf(url_out, urlcap, "%s", source);
    ref_out[0] = 0;
    return false;
}

/* 生成 zip 直链：有 ref → zipball/<ref>（tag/分支皆可）；
 * 无 ref → archive/HEAD.zip（默认分支）。ref 做百分号编码。 */
static void gh_zip_url(const GhRepo *g, const char *ref,
                       char *out, size_t cap) {
    if (!ref || !ref[0]) {
        snprintf(out, cap, "https://github.com/%s/%s/archive/HEAD.zip",
                 g->owner, g->repo);
        return;
    }
    char enc[256];
    size_t j = 0;
    for (size_t i = 0; ref[i] && j + 4 < sizeof enc; i++) {
        unsigned char c = (unsigned char)ref[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/') {
            enc[j++] = (char)c;
        } else {
            snprintf(enc + j, sizeof enc - j, "%%%02X", c);
            j += 3;
        }
    }
    enc[j] = 0;
    snprintf(out, cap, "https://github.com/%s/%s/zipball/%s",
             g->owner, g->repo, enc);
}

/* 解析 "v?X.Y.Z[-pre]"；-pre 后缀排在同号正式版之后。失败返回 false。 */
static bool semver_parse(const char *s, int *maj, int *min, int *pat,
                         int *pre) {
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    char *end;
    long a = strtol(s, &end, 10);
    if (end == s || *end != '.') return false;
    s = end + 1;
    long b = strtol(s, &end, 10);
    if (end == s || *end != '.') return false;
    s = end + 1;
    long c = strtol(s, &end, 10);
    if (end == s) return false;
    if (*end == '-' || *end == '_') *pre = 0;
    else if (*end == 0 || *end == '+' || *end == ' ' || *end == '\t')
        *pre = 1;
    else return false;
    if (a < 0 || a > 1000000 || b < 0 || b > 1000000 ||
        c < 0 || c > 1000000)
        return false;
    *maj = (int)a; *min = (int)b; *pat = (int)c;
    return true;
}

/* 从 tags JSON（[{"name":...}]）本地 semver 排序取最高 */
static bool gh_latest_from_tags(const char *json, size_t len,
                                char *out, size_t cap) {
    TVal v;
    if (!tphp_json_decode_c(json, len, &v)) return false;
    bool found = false;
    int bm = 0, bn = 0, bp = 0, bpre = 0;
    char best[128] = "";
    if (v.t == T_ARR) {
        TArr *arr = (TArr *)v.u.p;
        for (uint32_t i = 0; i < arr->dused; i++) {
            if (arr->data[i].t != T_ARR) continue;
            char *name = mget_str((TArr *)arr->data[i].u.p, "name");
            if (!name) continue;
            int m, n, p, pre;
            if (semver_parse(name, &m, &n, &p, &pre) &&
                (!found || m > bm || (m == bm && n > bn) ||
                 (m == bm && n == bn && p > bp) ||
                 (m == bm && n == bn && p == bp && pre > bpre))) {
                found = true;
                bm = m; bn = n; bp = p; bpre = pre;
                snprintf(best, sizeof best, "%s", name);
            }
            tm_free(name);
        }
    }
    tval_release(&v);
    if (!found) return false;
    snprintf(out, cap, "%s", best);
    return true;
}

/* 解析 GitHub 最新发布 ref（匿名 API，无认证；限流 60/h/IP）：
 * releases/latest 的 tag_name → 回退 tags 列表 semver 排序 → 都失败 false
 * （调用方回退默认分支）。网络/解析失败不致命。 */
static bool gh_latest_ref(const char *owner, const char *repo,
                          char *out, size_t cap) {
    if (!g_fetch) return false;
    char url[512], e2[256], *buf = NULL;
    size_t len = 0;

    snprintf(url, sizeof url,
             "https://api.github.com/repos/%s/%s/releases/latest",
             owner, repo);
    if (g_fetch(url, &buf, &len, e2, sizeof e2) == 0 && buf) {
        char *z = tm_malloc(len + 1);
        memcpy(z, buf, len);
        z[len] = 0;
        free(buf);
        buf = NULL;
        TVal v;
        if (tphp_json_decode_c(z, len, &v)) {
            if (v.t == T_ARR) {
                char *tag = mget_str((TArr *)v.u.p, "tag_name");
                if (tag && tag[0]) {
                    snprintf(out, cap, "%s", tag);
                    tm_free(tag);
                    tval_release(&v);
                    tm_free(z);
                    return true;
                }
                tm_free(tag);
            }
            tval_release(&v);
        }
        tm_free(z);
    } else if (buf) {
        free(buf);
        buf = NULL;
    }

    snprintf(url, sizeof url, "https://api.github.com/repos/%s/%s/tags",
             owner, repo);
    if (g_fetch(url, &buf, &len, e2, sizeof e2) == 0 && buf) {
        char *z = tm_malloc(len + 1);
        memcpy(z, buf, len);
        z[len] = 0;
        free(buf);
        bool ok = gh_latest_from_tags(z, len, out, cap);
        tm_free(z);
        return ok;
    }
    if (buf) free(buf);
    return false;
}

/* 路径拼接：a/b + c → a/b/c（a 可为空）。out 与 a/b 不得重叠。 */
static void path_join3(char *out, size_t cap, const char *a, const char *b) {
    if (!a || !a[0]) snprintf(out, cap, "%s", b);
    else snprintf(out, cap, "%s/%s", a, b);
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
}

/* 追加一段到路径：buf + seg → buf（buf 可能与 out 相同，故先拼临时区） */
static void path_append(char *out, size_t cap, const char *seg) {
    char tmp[1400];
    snprintf(tmp, sizeof tmp, "%s", out);
    path_join3(out, cap, tmp, seg);
}

/* 递归拷贝目录（跳过 .git / .DS_Store） */
static int fs_copy_tree(const char *src, const char *dst) {
    if (fs_mkdir_p(dst) < 0) return -1;
    uv_fs_t req;
    int r = uv_fs_scandir(NULL, &req, src, 0, NULL);
    if (r < 0) { uv_fs_req_cleanup(&req); return -1; }
    uv_dirent_t ent;
    int rc = 0;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;   /* .git .DS_Store .gitignore 等 */
        char sp[1200], dp[1200];
        snprintf(sp, sizeof sp, "%s/%s", src, ent.name);
        snprintf(dp, sizeof dp, "%s/%s", dst, ent.name);
        if (fs_isdir(sp)) {
            rc = fs_copy_tree(sp, dp);
        } else {
            size_t len = 0;
            char *data = fs_read(sp, &len);
            if (!data) { rc = -1; break; }
            rc = fs_write(dp, data, len);
            tm_free(data);
            if (rc < 0) break;
        }
    }
    uv_fs_req_cleanup(&req);
    return rc;
}

/* zip 解压到 dest（miniz；拒绝 .. 路径防 zip-slip）。返回 0 成功 */
static int zip_extract_mem(const void *data, size_t len, const char *dest,
                           char *err, size_t errcap) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_mem(&zip, data, len, 0)) {
        set_err(err, errcap, "invalid zip archive");
        return 2;
    }
    if (fs_mkdir_p(dest) < 0) {
        mz_zip_reader_end(&zip);
        set_err(err, errcap, "cannot create %s", dest);
        return 2;
    }
    mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        if (strstr(st.m_filename, "..") || st.m_filename[0] == '/') {
            mz_zip_reader_end(&zip);
            set_err(err, errcap, "unsafe path in zip: %s", st.m_filename);
            return 2;
        }
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dest, st.m_filename);
        /* 建父目录 */
        for (char *q = path + strlen(dest) + 1; *q; q++) {
            if (*q == '/') {
                *q = 0;
                fs_mkdir_p(path);
                *q = '/';
            }
        }
        size_t sz = 0;
        void *buf = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!buf) continue;
        fs_write(path, (const char *)buf, sz);
        free(buf);   /* miniz 用 malloc */
    }
    mz_zip_reader_end(&zip);
    return 0;
}

/* 在解压目录里找含 mod.json 的包根：直接命中，或唯一一级子目录命中
 * （GitHub archive 常见 repo-tag/ 前缀）。成功时 root 写入 out。 */
static int find_pkg_root(const char *dir, char *out, size_t cap) {
    char mj[1200];
    snprintf(mj, sizeof mj, "%s/mod.json", dir);
    if (fs_exists(mj)) { snprintf(out, cap, "%s", dir); return 0; }

    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, dir, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        return -1;
    }
    char found[1200] = "";
    int hits = 0;
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;
        char sub[1250];
        snprintf(sub, sizeof sub, "%s/%s", dir, ent.name);
        if (!fs_isdir(sub)) continue;
        char smj[1300];
        snprintf(smj, sizeof smj, "%s/mod.json", sub);
        if (fs_exists(smj)) {
            snprintf(found, sizeof found, "%s", sub);
            hits++;
        }
    }
    uv_fs_req_cleanup(&req);
    if (hits == 1) { snprintf(out, cap, "%s", found); return 0; }
    return -1;
}

/* 包 ID → vendor 路径：foo/bar → <proj>/vendor/foo/bar */
static void vendor_path_of(char *out, size_t cap, const char *proj,
                           const char *name) {
    path_join3(out, cap, proj, "vendor");
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", name);
    for (char *q = tmp; *q; q++) if (*q == '\\') *q = '/';
    char *p = tmp;
    while (*p) {
        char *slash = strchr(p, '/');
        if (!slash) { path_append(out, cap, p); break; }
        *slash = 0;
        path_append(out, cap, p);
        p = slash + 1;
    }
}

/* ---- mod.lock ---- */

/* ============================================================
 * lock integrity 快照（阶段 E）：vendor/<name>/ 全文件 sha1。
 * 写入单点 lock_save；校验点 = install 的 skip 判断 + build 打包
 * vendor 前（tphp_pkg_verify_vendor）。运行时不查。
 * ============================================================ */

typedef struct { char rel[1024]; char sha1[41]; } IntEntry;
typedef struct { IntEntry *v; int n, cap; } IntList;

static void intlist_push(IntList *l, const char *rel, const char *sha1) {
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = tm_realloc(l->v, sizeof(IntEntry) * (size_t)l->cap);
    }
    snprintf(l->v[l->n].rel, sizeof l->v[l->n].rel, "%s", rel);
    snprintf(l->v[l->n].sha1, sizeof l->v[l->n].sha1, "%s", sha1);
    l->n++;
}

static void intlist_free(IntList *l) {
    tm_free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* 递归扫描 dir：全部文件（跳过 '.' 开头条目）→ (rel, sha1)。rel 相对 dir
 * 根、'/' 分隔。返回 0；scandir 失败 -1。
 * skip_build：跳过名为 build 的子目录（ext 包编译产物，§6）。 */
static int integrity_scan_ex(const char *dir, const char *prefix, IntList *l,
                             bool skip_build) {
    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, dir, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        return -1;
    }
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;
        if (skip_build && strcmp(ent.name, "build") == 0) continue;
        char sub[1500], rel[1024];
        snprintf(sub, sizeof sub, "%s/%s", dir, ent.name);
        if (prefix[0]) snprintf(rel, sizeof rel, "%s/%s", prefix, ent.name);
        else snprintf(rel, sizeof rel, "%s", ent.name);
        if (fs_isdir(sub)) {
            if (integrity_scan_ex(sub, rel, l, skip_build) != 0) {
                uv_fs_req_cleanup(&req);
                return -1;
            }
        } else {
            char hex[41];
            if (tphp_sha1_file(sub, hex))
                intlist_push(l, rel, hex);
        }
    }
    uv_fs_req_cleanup(&req);
    return 0;
}

static int integrity_scan(const char *dir, const char *prefix, IntList *l) {
    return integrity_scan_ex(dir, prefix, l, false);
}

/* 把 lock 条目里的 integrity 对象展开成 IntList（拷贝，脱离 TArr 生命周期） */
static void integrity_from_snap(TArr *snap, IntList *l) {
    tarr_sync_buckets(snap);
    for (uint32_t i = 0; i < snap->dused; i++) {
        if (snap->data[i].t != T_STR) continue;
        TVal key = TV_NULL;
        for (uint32_t b = 0; b < snap->bcap; b++) {
            ABucket *bk = &snap->buckets[b];
            if (bk->key.t != T_NULL && bk->val_off == i) { key = bk->key; break; }
        }
        if (key.t != T_STR) continue;
        char rel[1024], hex[41];
        snprintf(rel, sizeof rel, "%s", tstr_val((TStr *)key.u.p));
        snprintf(hex, sizeof hex, "%s", tstr_val((TStr *)snap->data[i].u.p));
        intlist_push(l, rel, hex);
    }
}

/* 对照快照校验 vdir：快照每文件存在且 sha1 一致；磁盘无快照外新文件。
 * snap=NULL → 不校验（旧 lock 向后兼容）。0 通过；-1 不匹配（err 列出）。
 * skip_build：ext 包校验时跳过 build/（与 lock 写侧一致）。 */
static int integrity_check_ex(const char *vdir, TArr *snap, bool skip_build,
                              char *err, size_t errcap) {
    if (!snap) return 0;
    IntList expect = {0}, actual = {0};
    integrity_from_snap(snap, &expect);
    if (integrity_scan_ex(vdir, "", &actual, skip_build) != 0) {
        intlist_free(&expect);
        set_err(err, errcap, "cannot scan %s", vdir);
        return -1;
    }
    int bad = 0, shown_n = 0;
    char shown[512] = "";
    size_t sl = 0;
    /* 缺失 / 被改 */
    for (int i = 0; i < expect.n; i++) {
        const char *rel = expect.v[i].rel;
        const char *hex = expect.v[i].sha1;
        int j = 0;
        for (; j < actual.n; j++)
            if (strcmp(actual.v[j].rel, rel) == 0) break;
        bool mismatch;
        if (j >= actual.n) mismatch = true;              /* 缺失 */
        else mismatch = strcmp(actual.v[j].sha1, hex) != 0;
        if (!mismatch) continue;
        bad++;
        if (sl + strlen(rel) + 4 < sizeof shown) {
            sl += (size_t)snprintf(shown + sl, sizeof shown - sl, "%s%s",
                                   sl ? ", " : "", rel);
            shown_n++;
        }
    }
    /* 新增（快照外文件，挡侧载） */
    for (int i = 0; i < actual.n; i++) {
        int j = 0;
        for (; j < expect.n; j++)
            if (strcmp(expect.v[j].rel, actual.v[i].rel) == 0) break;
        if (j < expect.n) continue;
        bad++;
        if (sl + strlen(actual.v[i].rel) + 4 < sizeof shown) {
            sl += (size_t)snprintf(shown + sl, sizeof shown - sl, "%s%s",
                                   sl ? ", " : "", actual.v[i].rel);
            shown_n++;
        }
    }
    intlist_free(&expect);
    intlist_free(&actual);
    if (bad == 0) return 0;
    set_err(err, errcap, "%d file(s) mismatched: %s%s", bad, shown,
            bad > shown_n ? ", ..." : "");
    return -1;
}

/* vendor 路径默认不跳 build（兼容既有调用点） */
static int integrity_check(const char *vdir, TArr *snap,
                           char *err, size_t errcap) {
    return integrity_check_ex(vdir, snap, false, err, errcap);
}

/* 覆盖前对照 mod.lock ext[name].integrity 校验 destdir（§6）。
 * 无 lock/无条目/无快照 → 0（不校验，向后兼容）。
 * 0 通过；-1 不匹配；-2 读 lock 失败。 */
static int ext_overwrite_integrity(const char *proj_dir, const char *name,
                                   const char *destdir,
                                   char *err, size_t errcap) {
    char lockp[1250];
    snprintf(lockp, sizeof lockp, "%s/mod.lock", proj_dir);
    if (!fs_exists(lockp)) return 0;
    size_t len = 0;
    char *raw = fs_read(lockp, &len);
    if (!raw) return -2;
    char *stripped = jsonc_strip(raw, len);
    tm_free(raw);
    TVal lv;
    bool ok = tphp_json_decode_c(stripped, strlen(stripped), &lv);
    tm_free(stripped);
    if (!ok || lv.t != T_ARR) return 0;
    TArr *lock = (TArr *)lv.u.p;
    TArr *ent = NULL;
    TArr *ext = mget_obj(lock, "ext");
    if (ext) ent = mget_obj(ext, name);
    TArr *snap = ent ? mget_obj(ent, "integrity") : NULL;
    int rc = 0;
    if (snap)
        rc = integrity_check_ex(destdir, snap, true, err, errcap);
    tval_release(&lv);
    return rc < 0 ? -1 : 0;
}

static int lock_save(const char *proj_dir, char *err, size_t errcap) {
    char root_mj[1200];
    snprintf(root_mj, sizeof root_mj, "%s/mod.json", proj_dir);
    TMod root;
    if (!tmod_load(root_mj, &root, err, errcap)) return 2;

    PkgBuf b;
    pb_init(&b);
    pb_puts(&b, "{\n");

    /* BFS：root deps → 各包 manifest deps */
    char **names = NULL; char **urls = NULL; char **refs = NULL;
    int n = 0, cap = 0;
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", gmt);

    for (int i = 0; i < root.n_deps; i++) {
        if (n >= cap) {
            cap = cap ? cap * 2 : 16;
            names = tm_realloc(names, sizeof(char *) * (size_t)cap);
            urls = tm_realloc(urls, sizeof(char *) * (size_t)cap);
            refs = tm_realloc(refs, sizeof(char *) * (size_t)cap);
        }
        names[n] = xstrdup(root.deps[i].name);
        urls[n] = xstrdup(root.deps[i].url ? root.deps[i].url : "");
        refs[n] = xstrdup(root.deps[i].ref ? root.deps[i].ref : "");
        n++;
    }
    /* 展开传递依赖 */
    for (int i = 0; i < n; i++) {
        char vp[1400], pmj[1450];
        vendor_path_of(vp, sizeof vp, proj_dir, names[i]);
        snprintf(pmj, sizeof pmj, "%s/mod.json", vp);
        TMod pm;
        if (!tmod_load(pmj, &pm, NULL, 0)) continue;
        for (int j = 0; j < pm.n_deps; j++) {
            bool dup = false;
            for (int k = 0; k < n; k++)
                if (strcmp(names[k], pm.deps[j].name) == 0) { dup = true; break; }
            if (dup) continue;
            if (n >= cap) {
                cap = cap ? cap * 2 : 16;
                names = tm_realloc(names, sizeof(char *) * (size_t)cap);
                urls = tm_realloc(urls, sizeof(char *) * (size_t)cap);
                refs = tm_realloc(refs, sizeof(char *) * (size_t)cap);
            }
            names[n] = xstrdup(pm.deps[j].name);
            urls[n] = xstrdup(pm.deps[j].url ? pm.deps[j].url : "");
            refs[n] = xstrdup(pm.deps[j].ref ? pm.deps[j].ref : "");
            n++;
        }
        tmod_free(&pm);
    }

    for (int i = 0; i < n; i++) {
        pb_puts(&b, "  ");
        pb_json_str(&b, names[i]);
        pb_puts(&b, ": {\n    \"url\": ");
        pb_json_str(&b, urls[i]);
        pb_puts(&b, ",\n    \"ref\": ");
        pb_json_str(&b, refs[i]);
        pb_puts(&b, ",\n    \"resolved\": ");
        pb_json_str(&b, ts);
        /* integrity：vendor/<name>/ 已安装才写（阶段 E；旧 lock 无此段
         * = 不校验，下次成功 install/lock_save 自动补写） */
        char vp[1400];
        vendor_path_of(vp, sizeof vp, proj_dir, names[i]);
        if (fs_isdir(vp)) {
            IntList snap = {0};
            if (integrity_scan(vp, "", &snap) == 0 && snap.n > 0) {
                pb_puts(&b, ",\n    \"integrity\": {");
                for (int k = 0; k < snap.n; k++) {
                    pb_puts(&b, k ? ",\n      " : "\n      ");
                    pb_json_str(&b, snap.v[k].rel);
                    pb_puts(&b, ": ");
                    pb_json_str(&b, snap.v[k].sha1);
                }
                pb_puts(&b, "\n    }");
            }
            intlist_free(&snap);
        }
        pb_puts(&b, "\n  }");
        pb_puts(&b, (i + 1 < n || root.n_exts > 0) ? ",\n" : "\n");
        tm_free(names[i]); tm_free(urls[i]); tm_free(refs[i]);
    }
    tm_free(names); tm_free(urls); tm_free(refs);

    /* ext 分栏（§6）：保留键 "ext"（包 ID 必含 /，不会与 deps 撞名）。
     * 结构 = { name: {url,ref,resolved,integrity?} }；integrity 排除 build/。 */
    if (root.n_exts > 0) {
        char eroot[1024];
        bool have_root = tphp_pkg_ext_root(eroot, sizeof eroot);
        pb_puts(&b, "  \"ext\": {\n");
        for (int i = 0; i < root.n_exts; i++) {
            const ModDep *d = &root.exts[i];
            pb_puts(&b, "    ");
            pb_json_str(&b, d->name);
            pb_puts(&b, ": {\n      \"url\": ");
            pb_json_str(&b, d->url ? d->url : "");
            pb_puts(&b, ",\n      \"ref\": ");
            pb_json_str(&b, d->ref ? d->ref : "");
            pb_puts(&b, ",\n      \"resolved\": ");
            pb_json_str(&b, ts);
            if (have_root) {
                char ep[1450];
                snprintf(ep, sizeof ep, "%s/ext/%s", eroot, d->name);
                if (fs_isdir(ep)) {
                    IntList snap = {0};
                    if (integrity_scan_ex(ep, "", &snap, true) == 0 &&
                        snap.n > 0) {
                        pb_puts(&b, ",\n      \"integrity\": {");
                        for (int k = 0; k < snap.n; k++) {
                            pb_puts(&b, k ? ",\n        " : "\n        ");
                            pb_json_str(&b, snap.v[k].rel);
                            pb_puts(&b, ": ");
                            pb_json_str(&b, snap.v[k].sha1);
                        }
                        pb_puts(&b, "\n      }");
                    }
                    intlist_free(&snap);
                }
            }
            pb_puts(&b, "\n    }");
            pb_puts(&b, (i + 1 < root.n_exts) ? ",\n" : "\n");
        }
        pb_puts(&b, "  }");
        pb_puts(&b, "\n");
    }

    tmod_free(&root);

    pb_puts(&b, "}\n");
    char lockp[1250];
    snprintf(lockp, sizeof lockp, "%s/mod.lock", proj_dir);
    int rc = fs_write(lockp, b.p, b.len);
    tm_free(b.p);
    if (rc < 0) { set_err(err, errcap, "cannot write mod.lock"); return 2; }
    return 0;
}

/* ---- add ---- */

/* 内部安装：物化到 vendor/<name> + 递归传递依赖；不写根 deps。
 * out_name 可为 NULL；非 NULL 时写入包名（malloc，调用方 free）。
 * out_resolved_ref 可为 NULL；非 NULL 指向 128 字节缓冲，写入最终
 * 解析出的 ref（GitHub 无显式 ref 时 = 最新发布版）。 */
static int pkg_install_core(const char *proj_dir, const char *source,
                            const char *ref, char **out_name,
                            char *out_resolved_ref,
                            char *err, size_t errcap);

/* 递归安装依赖：url 为绝对 http(s) 或相对 proj_dir 的路径 */
static int install_deps(const char *proj_dir, const TMod *parent,
                        char *err, size_t errcap) {
    for (int i = 0; i < parent->n_deps; i++) {
        const char *durl = parent->deps[i].url;
        if (!durl || !durl[0]) continue;

        char exist[1400], emj[1450];
        vendor_path_of(exist, sizeof exist, proj_dir, parent->deps[i].name);
        snprintf(emj, sizeof emj, "%s/mod.json", exist);
        if (fs_exists(emj)) continue;

        char absurl[1400];
        if (is_http_url(durl) || durl[0] == '/' ||
            (strlen(durl) > 1 && durl[1] == ':')) {
            snprintf(absurl, sizeof absurl, "%s", durl);
        } else {
            path_join3(absurl, sizeof absurl, proj_dir, durl);
        }

        char e2[512] = "";
        int rc = pkg_install_core(proj_dir, absurl, parent->deps[i].ref,
                                  NULL, NULL, e2, sizeof e2);
        if (rc != 0) {
            set_err(err, errcap, "failed to install dependency %s: %s",
                    parent->deps[i].name, e2);
            return rc;
        }
    }
    return 0;
}

/* 覆盖安装前的安全检查（对齐 vpm fail-closed 思路）：目标目录不是
 * 「本包管理器装出来的包」就拒绝 rm -rf。
 *  - 不存在 → 可覆盖（首次安装）
 *  - 无 mod.json → 可能是用户数据/半成品，不是安装产物
 *  - 含 .git → 用户可能把它变成了手动 clone 的工作区，冲掉=丢代码
 *  - mod.json 的 name 与目标不符 → 同路径被换成了别的东西
 * 返回 NULL 可覆盖；否则返回拒绝原因（静态串）。 */
static const char *vendor_overwrite_blocked(const char *dest,
                                            const char *expect_name) {
    if (!fs_exists(dest)) return NULL;
    char mj[1450], git[1500];
    snprintf(mj, sizeof mj, "%s/mod.json", dest);
    if (!fs_exists(mj))
        return "no mod.json (not an installed package; delete it manually "
               "if sure)";
    snprintf(git, sizeof git, "%s/.git", dest);
    if (fs_exists(git))
        return "contains a .git work tree (commit or move your work, "
               "then delete the directory manually)";
    TMod cur;
    char e2[256];
    if (!tmod_load(mj, &cur, e2, sizeof e2))
        return "unreadable mod.json (delete the directory manually to proceed)";
    bool mismatch = strcmp(cur.name, expect_name) != 0;
    tmod_free(&cur);
    if (mismatch)
        return "holds a different package name (delete it manually to proceed)";
    return NULL;
}

/* 核心安装：把已物化的包目录 src 装到 vendor/<name> + 递归 deps。
 * out_name 可为 NULL；非 NULL 时写入包名（malloc，调用方 free）。 */
static int install_from_src(const char *proj_dir, const char *src,
                            char **out_name,
                            char *err, size_t errcap) {
    char smj[1300];
    snprintf(smj, sizeof smj, "%s/mod.json", src);
    TMod pkg;
    if (!tmod_load(smj, &pkg, err, errcap)) return 1;
    if (out_name) *out_name = xstrdup(pkg.name);

    char dest[1400];
    vendor_path_of(dest, sizeof dest, proj_dir, pkg.name);
    bool existed = false;
    {
        char emj[1450];
        snprintf(emj, sizeof emj, "%s/mod.json", dest);
        existed = fs_exists(emj);
    }
    const char *action = existed ? "update" : "install";

    ScriptCtx sc;
    memset(&sc, 0, sizeof sc);
    sc.action = action;
    sc.pkg_name = pkg.name;
    sc.pkg_version = pkg.version;
    sc.pkg_ref = "";   /* install_from_src 无 ref 上下文；add 层可补 */
    sc.project_dir = proj_dir;
    sc.is_root = 0;

    /* pre：install → src（新包脚本）；update → 旧 dest 的 preUpdate */
    {
        char e2[512] = "";
        int prc = 0;
        sc.phase = "pre";
        if (existed) {
            sc.pkg_dir = dest;
            prc = run_scripts_from_dir(dest, SC_PRE_UPDATE, &sc,
                                       e2, sizeof e2);
        } else {
            sc.pkg_dir = src;
            prc = run_script_slot(&pkg, SC_PRE_INSTALL, &sc,
                                  e2, sizeof e2);
        }
        if (prc != 0 && prc != 3) {
            set_err(err, errcap, "%s", e2[0] ? e2 : "pre script failed");
            tmod_free(&pkg);
            return prc == 1 ? 1 : 2;
        }
    }

    /* 覆盖安装：先删旧目录（有用户工作区嫌疑则拒绝，见上） */
    const char *block = vendor_overwrite_blocked(dest, pkg.name);
    if (block) {
        set_err(err, errcap, "refusing to overwrite %s: %s", dest, block);
        tmod_free(&pkg);
        return 2;
    }
    fs_rm_rf(dest);

    char src_fwd[1200];
    snprintf(src_fwd, sizeof src_fwd, "%s", src);
    path_fwd(src_fwd);
    if (strcmp(src_fwd, dest) != 0) {
        if (fs_copy_tree(src, dest) < 0) {
            set_err(err, errcap, "cannot copy package to %s", dest);
            tmod_free(&pkg);
            return 2;
        }
    }

    /* post：从 dest（已就位的新包）读 scripts */
    int post_rc = 0;
    {
        char e2[512] = "";
        sc.phase = "post";
        sc.pkg_dir = dest;
        int prc = run_scripts_from_dir(dest,
            existed ? SC_POST_UPDATE : SC_POST_INSTALL, &sc, e2, sizeof e2);
        if (prc == 3) {
            tphp_warn("PHP Warning: pkg post script failed for %s: %s\n",
                      pkg.name, e2);
            post_rc = 3;
        } else if (prc != 0) {
            tphp_warn("PHP Warning: pkg post script error for %s: %s\n",
                      pkg.name, e2);
        }
    }

    int rc = install_deps(proj_dir, &pkg, err, errcap);
    tmod_free(&pkg);
    if (rc != 0) return rc;
    return post_rc;   /* 3 = 完成但 post 失败（不回滚） */
}

/* 物化 source → 含 mod.json 的包根目录：本地目录直接用原路径（不复制），
 * zip / HTTP(GitHub) 解到 staging（调用方负责事后清理）。
 * PHP add 与 ext 命令共用这一条 fetch/解压/找根路径。
 * out_resolved_ref 可为 NULL；非 NULL 指向 128 字节缓冲（GitHub 无显式
 * ref 时 = 最新发布版）。 */
static int source_to_dir(const char *staging, const char *source,
                         const char *ref, char *out_root, size_t rootcap,
                         char *out_resolved_ref, char *err, size_t errcap) {
    fs_rm_rf(staging);
    if (is_http_url(source)) {
        if (!g_fetch) {
            set_err(err, errcap, "HTTP fetch not available");
            return 2;
        }
        /* GitHub 仓库页 URL：`:ref` 剥离 + 最新发布解析 → zip 直链改写；
         * mod.json 存规范化后的仓库 URL，ref 见 out_resolved_ref */
        char url_only[1400], urlref[128], ghurl[1400], latest[128];
        GhRepo g;
        bool is_gh = gh_classify(source, url_only, sizeof url_only,
                                 urlref, sizeof urlref, &g);
        /* 空 ref 统一为 NULL（urlref 是栈数组，空串指针非 NULL 会骗过 !ref） */
        const char *eff_ref = (ref && ref[0]) ? ref
                              : (urlref[0] ? urlref : NULL);
        const char *to_get = source;
        if (is_gh) {
            if (!eff_ref && g.treeref[0]) eff_ref = g.treeref;
            if (!eff_ref) {
                if (gh_latest_ref(g.owner, g.repo, latest, sizeof latest)) {
                    eff_ref = latest;
                } else {
                    tphp_warn("PHP Warning: pkg: cannot resolve "
                            "latest release for %s/%s, using default "
                            "branch\n", g.owner, g.repo);
                }
            }
            gh_zip_url(&g, eff_ref, ghurl, sizeof ghurl);
            to_get = ghurl;
        }
        if (out_resolved_ref)
            snprintf(out_resolved_ref, 128, "%s", eff_ref ? eff_ref : "");
        char *buf = NULL;
        size_t blen = 0;
        int rc = g_fetch(to_get, &buf, &blen, err, errcap);
        if (rc != 0) return rc;
        /* zip 魔数是 PK；以 '<' 开头只会是 HTML/JSON 等错误页 */
        if (blen > 0 && buf[0] == '<') {
            set_err(err, errcap,
                    "download of %s is not a zip (got an HTML page?)",
                    to_get);
            free(buf);
            return 2;
        }
        char zdir[1300];
        snprintf(zdir, sizeof zdir, "%s/zip", staging);
        rc = zip_extract_mem(buf, blen, zdir, err, errcap);
        free(buf);
        if (rc != 0) return rc;
        if (find_pkg_root(zdir, out_root, rootcap) != 0) {
            set_err(err, errcap, "no mod.json found in downloaded package");
            return 2;
        }
        return 0;
    }

    if (fs_isdir(source)) {
        char mj[1300];
        snprintf(mj, sizeof mj, "%s/mod.json", source);
        if (!fs_exists(mj)) {
            set_err(err, errcap, "no mod.json in %s", source);
            return 1;
        }
        snprintf(out_root, rootcap, "%s", source);
        return 0;
    }

    if (fs_exists(source)) {
        size_t zlen = 0;
        char *zbuf = fs_read(source, &zlen);
        if (!zbuf) {
            set_err(err, errcap, "cannot read %s", source);
            return 2;
        }
        char zdir[1300];
        snprintf(zdir, sizeof zdir, "%s/zip", staging);
        int rc = zip_extract_mem(zbuf, zlen, zdir, err, errcap);
        tm_free(zbuf);
        if (rc != 0) return rc;
        if (find_pkg_root(zdir, out_root, rootcap) != 0) {
            set_err(err, errcap, "no mod.json found in zip %s", source);
            return 2;
        }
        return 0;
    }

    set_err(err, errcap, "source not found: %s", source);
    return 1;
}

static int pkg_install_core(const char *proj_dir, const char *source,
                            const char *ref, char **out_name,
                            char *out_resolved_ref,
                            char *err, size_t errcap) {
    char staging[1250];
    snprintf(staging, sizeof staging, "%s/.pkg-staging", proj_dir);
    char root[1300];
    int rc = source_to_dir(staging, source, ref, root, sizeof root,
                           out_resolved_ref, err, errcap);
    if (rc != 0) { fs_rm_rf(staging); return rc; }
    rc = install_from_src(proj_dir, root, out_name, err, errcap);
    fs_rm_rf(staging);
    return rc;
}

int tphp_pkg_add(const char *proj_dir, const char *source, const char *ref,
                 char *err, size_t errcap) {
    if (!proj_dir || !proj_dir[0] || !source || !source[0]) {
        set_err(err, errcap, "add requires project dir and source");
        return 1;
    }
    char root_mj[1200];
    snprintf(root_mj, sizeof root_mj, "%s/mod.json", proj_dir);
    if (!fs_exists(root_mj)) {
        set_err(err, errcap, "no mod.json in %s (not a package project?)", proj_dir);
        return 1;
    }

    /* 规范化：剥 `:ref` 后缀；ref 优先级 CLI > URL 后缀；GitHub 无 ref
     * 时 install core 解析最新发布并回填 resolved */
    char url_only[1400], urlref[128], resolved[128] = "";
    GhRepo g;
    gh_classify(source, url_only, sizeof url_only, urlref, sizeof urlref, &g);
    const char *eff = (ref && ref[0]) ? ref : urlref;

    /* 根 pre*：按 deps 是否已存在区分 install / update */
    TMod root_pre;
    bool have_root = tmod_load(root_mj, &root_pre, err, errcap);
    bool is_upd = false;
    if (have_root) {
        /* 粗判：源路径/URL 与现有 dep 相同或 vendor 已有同名（名未知时
         * 用 vendor 扫描过重——v1：deps 里 url 相同即视为 update） */
        for (int i = 0; i < root_pre.n_deps; i++) {
            if (root_pre.deps[i].url &&
                strcmp(root_pre.deps[i].url, url_only) == 0) {
                is_upd = true;
                break;
            }
        }
    }
    if (have_root) {
        ScriptCtx sc;
        memset(&sc, 0, sizeof sc);
        sc.action = is_upd ? "update" : "install";
        sc.phase = "pre";
        sc.pkg_name = root_pre.name;
        sc.pkg_version = root_pre.version;
        sc.pkg_dir = proj_dir;
        sc.project_dir = proj_dir;
        sc.is_root = 1;
        int prc = run_script_slot(&root_pre,
            is_upd ? SC_PRE_UPDATE : SC_PRE_INSTALL, &sc, err, errcap);
        if (prc != 0 && prc != 3) {
            tmod_free(&root_pre);
            return prc == 1 ? 1 : 2;
        }
        tmod_free(&root_pre);
    }

    char *pkg_name = NULL;
    int rc = pkg_install_core(proj_dir, source, eff, &pkg_name, resolved,
                              err, errcap);
    int post_fail = 0;
    if (rc == 3) {
        /* 包已装好，仅 post 钩子失败：继续写 deps/lock，最后返回 3 */
        post_fail = 3;
        rc = 0;
    }
    if (rc != 0) { tm_free(pkg_name); return rc; }

    const char *store_ref = (ref && ref[0]) ? ref
                            : (urlref[0] ? urlref : resolved);

    /* 写入根 deps（已存在 = 更新该包，url/ref 同步刷新为本次解析结果） */
    TMod root;
    if (!tmod_load(root_mj, &root, err, errcap)) {
        tm_free(pkg_name);
        return 2;
    }
    bool found = false;
    for (int i = 0; i < root.n_deps; i++) {
        if (strcmp(root.deps[i].name, pkg_name) != 0) continue;
        found = true;
        tm_free(root.deps[i].url);
        tm_free(root.deps[i].ref);
        root.deps[i].url = xstrdup(url_only);
        root.deps[i].ref = xstrdup(store_ref);
        break;
    }
    if (!found) {
        root.deps = tm_realloc(root.deps,
            sizeof(ModDep) * (size_t)(root.n_deps + 1));
        root.deps[root.n_deps].name = pkg_name;   /* 所有权转移 */
        root.deps[root.n_deps].url = xstrdup(url_only);
        root.deps[root.n_deps].ref = xstrdup(store_ref);
        root.n_deps++;
        pkg_name = NULL;
    } else {
        tm_free(pkg_name);
    }
    if (tmod_save(root_mj, &root, err, errcap) != 0) {
        tmod_free(&root);
        return 2;
    }

    lock_save(proj_dir, err, errcap);
    tphp_pkg_gen_autoload(proj_dir, err, errcap);

    /* 根 post*（autoload 已重写） */
    {
        ScriptCtx sc;
        memset(&sc, 0, sizeof sc);
        sc.action = is_upd ? "update" : "install";
        sc.phase = "post";
        sc.pkg_name = root.name;
        sc.pkg_version = root.version;
        sc.pkg_dir = proj_dir;
        sc.project_dir = proj_dir;
        sc.is_root = 1;
        int prc = run_script_slot(&root,
            is_upd ? SC_POST_UPDATE : SC_POST_INSTALL, &sc, err, errcap);
        tmod_free(&root);
        if (prc == 3) post_fail = 3;
        else if (prc != 0) return prc == 1 ? 1 : 2;
    }
    return post_fail;   /* 0 ok；3 = 完成但 post 失败 */
}

/* ---- remove ---- */

/* 从 proj 的 vendor 与 root deps 移除 name；清理不可达的传递依赖 */
int tphp_pkg_remove(const char *proj_dir, const char *name,
                    char *err, size_t errcap) {
    if (!proj_dir || !proj_dir[0] || !name || !name[0]) {
        set_err(err, errcap, "remove requires project dir and package name");
        return 1;
    }
    char root_mj[1200];
    snprintf(root_mj, sizeof root_mj, "%s/mod.json", proj_dir);
    TMod root;
    if (!tmod_load(root_mj, &root, err, errcap)) return 1;

    bool found = false;
    for (int i = 0; i < root.n_deps; i++) {
        if (strcmp(root.deps[i].name, name) != 0) continue;
        found = true;
        tm_free(root.deps[i].name);
        tm_free(root.deps[i].url);
        tm_free(root.deps[i].ref);
        for (int j = i; j + 1 < root.n_deps; j++)
            root.deps[j] = root.deps[j + 1];
        root.n_deps--;
        break;
    }
    if (!found) {
        set_err(err, errcap, "\"%s\" is not a dependency", name);
        tmod_free(&root);
        return 1;
    }

    /* 根 preRemove */
    {
        ScriptCtx sc;
        memset(&sc, 0, sizeof sc);
        sc.action = "remove";
        sc.phase = "pre";
        sc.pkg_name = root.name;
        sc.pkg_version = root.version;
        sc.pkg_dir = proj_dir;
        sc.project_dir = proj_dir;
        sc.is_root = 1;
        int prc = run_script_slot(&root, SC_PRE_REMOVE, &sc, err, errcap);
        if (prc != 0 && prc != 3) {
            tmod_free(&root);
            return prc == 1 ? 1 : 2;
        }
    }

    /* 标记删除：从 root deps 出发 BFS 可达集；不在集内的 vendor 包删掉 */
    char **keep = NULL;
    int nkeep = 0, ckeep = 0;
    for (int i = 0; i < root.n_deps; i++) {
        if (nkeep >= ckeep) {
            ckeep = ckeep ? ckeep * 2 : 16;
            keep = tm_realloc(keep, sizeof(char *) * (size_t)ckeep);
        }
        keep[nkeep++] = root.deps[i].name;
    }
    /* 展开 keep（各包 deps） */
    for (int i = 0; i < nkeep; i++) {
        char vp[1400], pmj[1450];
        vendor_path_of(vp, sizeof vp, proj_dir, keep[i]);
        snprintf(pmj, sizeof pmj, "%s/mod.json", vp);
        TMod pm;
        if (!tmod_load(pmj, &pm, NULL, 0)) continue;
        for (int j = 0; j < pm.n_deps; j++) {
            bool dup = false;
            for (int k = 0; k < nkeep; k++)
                if (strcmp(keep[k], pm.deps[j].name) == 0) { dup = true; break; }
            if (dup) continue;
            if (nkeep >= ckeep) {
                ckeep = ckeep ? ckeep * 2 : 16;
                keep = tm_realloc(keep, sizeof(char *) * (size_t)ckeep);
            }
            keep[nkeep++] = xstrdup(pm.deps[j].name);
        }
        tmod_free(&pm);
    }

    /* 删除目标 + 所有不在 keep 里的 vendor 包 */
    char tgt[1400];
    vendor_path_of(tgt, sizeof tgt, proj_dir, name);
    {
        ScriptCtx sc;
        memset(&sc, 0, sizeof sc);
        sc.action = "remove";
        sc.phase = "pre";
        sc.pkg_name = name;
        sc.pkg_dir = tgt;
        sc.project_dir = proj_dir;
        sc.is_root = 0;
        char e2[512] = "";
        int prc = run_scripts_from_dir(tgt, SC_PRE_REMOVE, &sc,
                                       e2, sizeof e2);
        if (prc != 0 && prc != 3) {
            set_err(err, errcap, "%s", e2[0] ? e2 : "preRemove failed");
            for (int i = 0; i < nkeep; i++) {
                for (int j = 0; j < root.n_deps; j++)
                    if (keep[i] == root.deps[j].name) { keep[i] = NULL; break; }
                if (keep[i]) tm_free(keep[i]);
            }
            tm_free(keep);
            tmod_free(&root);
            return prc == 1 ? 1 : 2;
        }
    }
    fs_rm_rf(tgt);

    char vdir[1250];
    snprintf(vdir, sizeof vdir, "%s/vendor", proj_dir);
    /* 先收集待删路径，再统一删除（scandir 迭代中删目录会丢条目） */
    char **todel = NULL;
    int ntodel = 0, ctodel = 0;
    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, vdir, 0, NULL) >= 0) {
        uv_dirent_t ent;
        while (uv_fs_scandir_next(&req, &ent) == 0) {
            if (ent.name[0] == '.') continue;
            char sub[1300];
            snprintf(sub, sizeof sub, "%s/%s", vdir, ent.name);
            if (!fs_isdir(sub)) continue;
            /* 一级：vendor/<a> —— 可能是单段包或两级包的 vendor 段 */
            char amj[1350];
            snprintf(amj, sizeof amj, "%s/mod.json", sub);
            if (fs_exists(amj)) {
                /* 单段包名 */
                bool in_keep = false;
                char id[1300];
                snprintf(id, sizeof id, "%s", ent.name);
                for (int k = 0; k < nkeep; k++)
                    if (strcmp(keep[k], id) == 0) { in_keep = true; break; }
                if (!in_keep) {
                    if (ntodel >= ctodel) {
                        ctodel = ctodel ? ctodel * 2 : 8;
                        todel = tm_realloc(todel, sizeof(char *) * (size_t)ctodel);
                    }
                    todel[ntodel++] = xstrdup(sub);
                }
                continue;
            }
            uv_fs_t r2;
            if (uv_fs_scandir(NULL, &r2, sub, 0, NULL) < 0) {
                uv_fs_req_cleanup(&r2);
                continue;
            }
            uv_dirent_t e2;
            while (uv_fs_scandir_next(&r2, &e2) == 0) {
                if (e2.name[0] == '.') continue;
                char pkg[1400];
                snprintf(pkg, sizeof pkg, "%s/%s", sub, e2.name);
                char pmj[1450];
                snprintf(pmj, sizeof pmj, "%s/mod.json", pkg);
                if (!fs_exists(pmj)) continue;
                char id[1300];
                snprintf(id, sizeof id, "%s/%s", ent.name, e2.name);
                bool in_keep = false;
                for (int k = 0; k < nkeep; k++)
                    if (strcmp(keep[k], id) == 0) { in_keep = true; break; }
                if (!in_keep) {
                    if (ntodel >= ctodel) {
                        ctodel = ctodel ? ctodel * 2 : 8;
                        todel = tm_realloc(todel, sizeof(char *) * (size_t)ctodel);
                    }
                    todel[ntodel++] = xstrdup(pkg);
                }
            }
            uv_fs_req_cleanup(&r2);
        }
    }
    uv_fs_req_cleanup(&req);
    for (int i = 0; i < ntodel; i++) {
        ScriptCtx sc;
        memset(&sc, 0, sizeof sc);
        sc.action = "remove";
        sc.phase = "pre";
        sc.pkg_dir = todel[i];
        sc.project_dir = proj_dir;
        sc.is_root = 0;
        run_scripts_from_dir(todel[i], SC_PRE_REMOVE, &sc, NULL, 0);
        fs_rm_rf(todel[i]);
        tm_free(todel[i]);
    }
    if (todel) tm_free(todel);
    for (int i = 0; i < nkeep; i++) {
        /* keep[0..nkeep) 中 root.deps 的指针归 root 所有；xstrdup 的需释放 */
        for (int j = 0; j < root.n_deps; j++)
            if (keep[i] == root.deps[j].name) { keep[i] = NULL; break; }
        if (keep[i]) tm_free(keep[i]);
    }
    tm_free(keep);

    /* 保存 + lock + autoload */
    if (tmod_save(root_mj, &root, err, errcap) != 0) {
        tmod_free(&root);
        return 2;
    }
    lock_save(proj_dir, err, errcap);
    tphp_pkg_gen_autoload(proj_dir, err, errcap);

    /* 根 postRemove（包已删，仅根） */
    {
        ScriptCtx sc;
        memset(&sc, 0, sizeof sc);
        sc.action = "remove";
        sc.phase = "post";
        sc.pkg_name = root.name;
        sc.pkg_version = root.version;
        sc.pkg_dir = proj_dir;
        sc.project_dir = proj_dir;
        sc.is_root = 1;
        int prc = run_script_slot(&root, SC_POST_REMOVE, &sc, err, errcap);
        tmod_free(&root);
        if (prc == 3) return 3;
        if (prc != 0) return prc == 1 ? 1 : 2;
    }
    return 0;
}

/* ---- install（按 mod.lock 恢复；无 lock 则从 mod.json deps 全量） ---- */

int tphp_pkg_install(const char *proj_dir, bool force,
                     char *err, size_t errcap) {
    if (!proj_dir || !proj_dir[0]) {
        set_err(err, errcap, "install requires project dir");
        return 1;
    }
    int post_fail_any = 0;
    char root_mj[1200], lockp[1250];
    snprintf(root_mj, sizeof root_mj, "%s/mod.json", proj_dir);
    snprintf(lockp, sizeof lockp, "%s/mod.lock", proj_dir);
    if (!fs_exists(root_mj)) {
        set_err(err, errcap, "no mod.json in %s (not a package project?)", proj_dir);
        return 1;
    }

    /* 根 pre*：有 lock 且存在已装包 + force → update；否则 install。
     * 简化：force 视为 update，否则 install（新机器补装）。 */
    TMod root_pre;
    if (tmod_load(root_mj, &root_pre, err, errcap)) {
        ScriptCtx sc;
        memset(&sc, 0, sizeof sc);
        sc.action = force ? "update" : "install";
        sc.phase = "pre";
        sc.pkg_name = root_pre.name;
        sc.pkg_version = root_pre.version;
        sc.pkg_dir = proj_dir;
        sc.project_dir = proj_dir;
        sc.is_root = 1;
        int prc = run_script_slot(&root_pre,
            force ? SC_PRE_UPDATE : SC_PRE_INSTALL, &sc, err, errcap);
        if (prc != 0 && prc != 3) {
            tmod_free(&root_pre);
            return prc == 1 ? 1 : 2;
        }
        tmod_free(&root_pre);
    }

    /* lock 存在：逐条恢复缺失包；否则从 mod.json deps 递归 */
    if (fs_exists(lockp)) {
        size_t llen = 0;
        char *lraw = fs_read(lockp, &llen);
        if (!lraw) {
            set_err(err, errcap, "cannot read mod.lock");
            return 2;
        }
        char *lstripped = jsonc_strip(lraw, llen);
        tm_free(lraw);
        TVal lv;
        bool ok = tphp_json_decode_c(lstripped, strlen(lstripped), &lv);
        tm_free(lstripped);
        if (!ok || lv.t != T_ARR) {
            set_err(err, errcap, "mod.lock: invalid JSON");
            return 2;
        }
        TArr *lock = (TArr *)lv.u.p;
        tarr_sync_buckets(lock);
        for (uint32_t i = 0; i < lock->dused; i++) {
            if (lock->data[i].t != T_ARR) continue;
            TVal key = TV_NULL;
            for (uint32_t b = 0; b < lock->bcap; b++) {
                ABucket *bk = &lock->buckets[b];
                if (bk->key.t != T_NULL && bk->val_off == i) { key = bk->key; break; }
            }
            if (key.t != T_STR) continue;
            TStr *ks = (TStr *)key.u.p;
            TArr *ent = (TArr *)lock->data[i].u.p;
            char *url = mget_str(ent, "url");
            char *ref = mget_str(ent, "ref");
            if (!url || !url[0]) { tm_free(url); tm_free(ref); continue; }

            /* 已安装：有 integrity 快照则逐文件校验（阶段 E；旧 lock 无
             * 快照 = 不校验，向后兼容）。不匹配 fail-closed：--force 重装，
             * 否则报错（不静默冲掉本地改动）。 */
            char exist[1400], emj[1450];
            vendor_path_of(exist, sizeof exist, proj_dir, tstr_val(ks));
            snprintf(emj, sizeof emj, "%s/mod.json", exist);
            if (fs_exists(emj)) {
                TArr *snap = mget_obj(ent, "integrity");
                char e2[512] = "";
                if (!snap || integrity_check(exist, snap, e2, sizeof e2) == 0) {
                    tm_free(url); tm_free(ref);
                    continue;                       /* 校验通过 → skip */
                }
                if (!force) {
                    set_err(err, errcap,
                            "integrity check failed for %s: %s "
                            "(use install --force to reinstall)",
                            tstr_val(ks), e2);
                    tval_release(&lv);
                    tm_free(url); tm_free(ref);
                    return 2;
                }
                fs_rm_rf(exist);                    /* --force：删了重装 */
            }

            char absurl[1400];
            if (is_http_url(url) || url[0] == '/' ||
                (strlen(url) > 1 && url[1] == ':')) {
                snprintf(absurl, sizeof absurl, "%s", url);
            } else {
                path_join3(absurl, sizeof absurl, proj_dir, url);
            }
            char e2[512] = "";
            int rc = pkg_install_core(proj_dir, absurl, ref, NULL, NULL,
                                      e2, sizeof e2);
            if (rc == 3) { post_fail_any = 1; rc = 0; }
            if (rc != 0) {
                set_err(err, errcap, "install %s failed: %s", tstr_val(ks), e2);
                tval_release(&lv);
                tm_free(url); tm_free(ref);
                return rc;
            }
            tm_free(url); tm_free(ref);
        }
        tval_release(&lv);
    } else {
        /* 无 lock：从 mod.json deps 递归安装 */
        TMod root;
        if (!tmod_load(root_mj, &root, err, errcap)) return 2;
        for (int i = 0; i < root.n_deps; i++) {
            char exist[1400], emj[1450];
            vendor_path_of(exist, sizeof exist, proj_dir, root.deps[i].name);
            snprintf(emj, sizeof emj, "%s/mod.json", exist);
            if (fs_exists(emj)) continue;
            char absurl[1400];
            const char *durl = root.deps[i].url;
            if (!durl || !durl[0]) continue;
            if (is_http_url(durl) || durl[0] == '/' ||
                (strlen(durl) > 1 && durl[1] == ':')) {
                snprintf(absurl, sizeof absurl, "%s", durl);
            } else {
                path_join3(absurl, sizeof absurl, proj_dir, durl);
            }
            char e2[512] = "";
            int rc = pkg_install_core(proj_dir, absurl, root.deps[i].ref,
                                      NULL, NULL, e2, sizeof e2);
            if (rc == 3) { post_fail_any = 1; rc = 0; }
            if (rc != 0) {
                set_err(err, errcap, "install %s failed: %s",
                        root.deps[i].name, e2);
                tmod_free(&root);
                return rc;
            }
        }
        tmod_free(&root);
    }

    lock_save(proj_dir, err, errcap);
    tphp_pkg_gen_autoload(proj_dir, err, errcap);

    /* 根 post* */
    {
        TMod root_post;
        if (tmod_load(root_mj, &root_post, NULL, 0)) {
            ScriptCtx sc;
            memset(&sc, 0, sizeof sc);
            sc.action = force ? "update" : "install";
            sc.phase = "post";
            sc.pkg_name = root_post.name;
            sc.pkg_version = root_post.version;
            sc.pkg_dir = proj_dir;
            sc.project_dir = proj_dir;
            sc.is_root = 1;
            int prc = run_script_slot(&root_post,
                force ? SC_POST_UPDATE : SC_POST_INSTALL, &sc, err, errcap);
            tmod_free(&root_post);
            if (prc == 3) post_fail_any = 1;
            else if (prc != 0) return prc == 1 ? 1 : 2;
        }
    }
    return post_fail_any ? 3 : 0;
}

/* build 自动打包 vendor 前的 integrity 卡口（阶段 E）：读 mod.lock，
 * 对每个带 integrity 快照的已装包逐文件校验。无 lock / 条目无快照 →
 * 跳过（向后兼容与手搓项目警告放行）。0 通过；2 不匹配。 */
int tphp_pkg_verify_vendor(const char *proj_dir, char *err, size_t errcap) {
    char lockp[1250];
    snprintf(lockp, sizeof lockp, "%s/mod.lock", proj_dir);
    if (!fs_exists(lockp)) return 0;                 /* 无 lock：放行 */
    size_t llen = 0;
    char *lraw = fs_read(lockp, &llen);
    if (!lraw) {
        set_err(err, errcap, "cannot read mod.lock");
        return 2;
    }
    char *lstripped = jsonc_strip(lraw, llen);
    tm_free(lraw);
    TVal lv;
    bool ok = tphp_json_decode_c(lstripped, strlen(lstripped), &lv);
    tm_free(lstripped);
    if (!ok || lv.t != T_ARR) return 0;              /* lock 坏：放行交给 install 流程 */
    TArr *lock = (TArr *)lv.u.p;
    int rc = 0;
    tarr_sync_buckets(lock);
    for (uint32_t i = 0; i < lock->dused; i++) {
        if (lock->data[i].t != T_ARR) continue;
        TVal key = TV_NULL;
        for (uint32_t b = 0; b < lock->bcap; b++) {
            ABucket *bk = &lock->buckets[b];
            if (bk->key.t != T_NULL && bk->val_off == i) { key = bk->key; break; }
        }
        if (key.t != T_STR) continue;
        TArr *ent = (TArr *)lock->data[i].u.p;
        TArr *snap = mget_obj(ent, "integrity");
        if (!snap) continue;
        char exist[1400];
        vendor_path_of(exist, sizeof exist, proj_dir, tstr_val((TStr *)key.u.p));
        if (!fs_isdir(exist)) continue;              /* 未装：不在此拦 */
        char e2[512] = "";
        if (integrity_check(exist, snap, e2, sizeof e2) != 0) {
            set_err(err, errcap, "vendor integrity check failed for %s: %s "
                    "(re-run tphp install --force before building)",
                    tstr_val((TStr *)key.u.p), e2);
            rc = 2;
            break;
        }
    }
    tval_release(&lv);
    return rc;
}

/* ================================================================
 * ext 命令组（PLAN/ext-packages.md §4）+ 编译链入（§5，切片 5-6 完成）
 * 落点 <tphp_root>/ext/<name>；项目模式写回 ext[]；add 恒编译重链（11-12
 * 拍板：改源再 add 即出新 tphp），sync/update 未链入时编译。
 * 铁律（§10.9）：不进 vendor/、不进 autoload、无 scripts。
 * ================================================================ */

typedef struct {
    char name[256];   /* 来自源包 mod.json（双段 ID 已校验） */
    char url[1400];   /* 规范化：GitHub 剥 :ref；其余按输入原样 */
    char ref[128];    /* 生效 ref：CLI > :ref > 解析结果（如最新发布） */
} ExtInfo;

/* §5 编译管线（定义在文件尾） */
static int ext_compile_all(char *err, size_t errcap);
static bool ext_built_has(const char *eroot, const char *name);

/* <tphp_root>/ext/<name>；无法定位 tphp root 返回 false */
static bool ext_pkg_dir(const char *name, char *out, size_t cap) {
    char root[1024];
    if (!tphp_pkg_ext_root(root, sizeof root)) return false;
    snprintf(out, cap, "%s/ext/%s", root, name);
    return true;
}

/* §4.1 项目判定：proj_dir/mod.json 存在且能加载，且不是 ext 包本身
 *（type=ext 清单没有可写的 ext[]）。rc：0 = *out_is_proj 已置位；
 * 2 = 清单存在但读不开（坏清单大声失败）。 */
static int ext_proj_mode(const char *proj_dir, bool *out_is_proj,
                         char *err, size_t errcap) {
    *out_is_proj = false;
    if (!proj_dir || !proj_dir[0]) return 0;
    char mj[1250];
    snprintf(mj, sizeof mj, "%s/mod.json", proj_dir);
    if (!fs_exists(mj)) return 0;
    TMod m;
    if (!tmod_load(mj, &m, err, errcap)) return 2;
    *out_is_proj = !m.has_ext_spec;
    tmod_free(&m);
    return 0;
}

/* ext[] 条目 url → 绝对取值：http/绝对路径原样，其余相对 proj_dir（§3） */
static void ext_abs_url(const char *proj_dir, const char *url,
                        char *out, size_t cap) {
    if (is_http_url(url) || url[0] == '/' ||
        (strlen(url) > 1 && url[1] == ':')) {
        snprintf(out, cap, "%s", url);
    } else {
        path_join3(out, cap, proj_dir, url);
    }
}

/* 物化 source → <tphp_root>/ext/<name>：
 *  type=ext 强校验（§2.1；tmod_load 已含禁止项与契约必填）+ 双段名；
 *  expect_name 非 NULL → 源包名必须一致（sync/update 声明对账，防装错位）；
 *  覆盖安全 = vendor 同款三查（mod.json / .git / 名字符合）。 */
static int ext_install(const char *source, const char *ref,
                       const char *expect_name, const char *proj_dir,
                       bool force, ExtInfo *out,
                       char *err, size_t errcap) {
    char eroot[1024];
    if (!tphp_pkg_ext_root(eroot, sizeof eroot)) {
        set_err(err, errcap, "cannot locate tphp root (self exe unknown)");
        return 2;
    }
    char staging[1300];
    snprintf(staging, sizeof staging, "%s/ext/.staging", eroot);

    char url_only[1400], urlref[128], resolved[128] = "";
    GhRepo g;
    gh_classify(source, url_only, sizeof url_only, urlref, sizeof urlref, &g);

    char root[1300];
    int rc = source_to_dir(staging, source, ref, root, sizeof root,
                           resolved, err, errcap);
    if (rc != 0) { fs_rm_rf(staging); return rc; }

    char mj[1400];
    TMod pkg;
    snprintf(mj, sizeof mj, "%s/mod.json", root);
    if (!tmod_load(mj, &pkg, err, errcap)) { fs_rm_rf(staging); return 2; }
    if (!pkg.has_ext_spec) {
        set_err(err, errcap, "not an ext package (type \"%s\"), expected \"ext\"",
                pkg.type ? pkg.type : "?");
        tmod_free(&pkg); fs_rm_rf(staging); return 1;
    }
    const char *why = NULL;
    if (!tphp_pkg_valid_name(pkg.name, true, &why)) {
        set_err(err, errcap, "invalid ext name \"%s\": %s (need vendor/package)",
                pkg.name, why ? why : "");
        tmod_free(&pkg); fs_rm_rf(staging); return 1;
    }
    if (expect_name && strcmp(pkg.name, expect_name) != 0) {
        set_err(err, errcap,
                "ext name mismatch: declared \"%s\" but source is \"%s\"",
                expect_name, pkg.name);
        tmod_free(&pkg); fs_rm_rf(staging); return 1;
    }

    char dest[1400];
    snprintf(dest, sizeof dest, "%s/ext/%s", eroot, pkg.name);
    const char *block = vendor_overwrite_blocked(dest, pkg.name);
    if (block) {
        set_err(err, errcap, "refusing to overwrite %s: %s", dest, block);
        tmod_free(&pkg); fs_rm_rf(staging); return 2;
    }

    /* 覆盖前 integrity（§6）：有项目 lock 才校验；force（--update）跳过 */
    if (!force && proj_dir && proj_dir[0] && fs_exists(dest)) {
        char e2[512] = "";
        if (ext_overwrite_integrity(proj_dir, pkg.name, dest,
                                    e2, sizeof e2) != 0) {
            set_err(err, errcap, "integrity check failed for ext %s: %s "
                    "(delete %s manually or use --update to overwrite)",
                    pkg.name, e2[0] ? e2 : "mismatch", dest);
            tmod_free(&pkg); fs_rm_rf(staging); return 2;
        }
    }

    /* source 就是目标目录（原地重装）→ 不复制 */
    char d_fwd[1400], s_fwd[1400];
    snprintf(d_fwd, sizeof d_fwd, "%s", dest);
    snprintf(s_fwd, sizeof s_fwd, "%s", root);
    path_fwd(d_fwd);
    path_fwd(s_fwd);
    if (strcmp(d_fwd, s_fwd) != 0) {
        fs_rm_rf(dest);
        if (fs_copy_tree(root, dest) < 0) {
            set_err(err, errcap, "cannot install ext to %s", dest);
            tmod_free(&pkg); fs_rm_rf(staging); return 2;
        }
    }

    if (out) {
        snprintf(out->name, sizeof out->name, "%s", pkg.name);
        snprintf(out->url, sizeof out->url, "%s", url_only);
        const char *eff = (ref && ref[0]) ? ref
                          : (urlref[0] ? urlref : resolved);
        snprintf(out->ref, sizeof out->ref, "%s", eff ? eff : "");
    }
    tmod_free(&pkg);
    fs_rm_rf(staging);
    return 0;
}

/* 项目 ext[] 写回（§4.1）：同 name 更新 url/ref，否则追加 */
static int ext_writeback(const char *proj_dir, const ExtInfo *info,
                         char *err, size_t errcap) {
    char mj[1250];
    snprintf(mj, sizeof mj, "%s/mod.json", proj_dir);
    TMod root;
    if (!tmod_load(mj, &root, err, errcap)) return 2;
    bool found = false;
    for (int i = 0; i < root.n_exts; i++) {
        if (strcmp(root.exts[i].name, info->name) != 0) continue;
        found = true;
        tm_free(root.exts[i].url);
        tm_free(root.exts[i].ref);
        root.exts[i].url = xstrdup(info->url);
        root.exts[i].ref = xstrdup(info->ref);
        break;
    }
    if (!found) {
        root.exts = tm_realloc(root.exts,
                               sizeof(ModDep) * (size_t)(root.n_exts + 1));
        root.exts[root.n_exts].name = xstrdup(info->name);
        root.exts[root.n_exts].url = xstrdup(info->url);
        root.exts[root.n_exts].ref = xstrdup(info->ref);
        root.n_exts++;
    }
    int rc = tmod_save(mj, &root, err, errcap);
    tmod_free(&root);
    if (rc != 0) return 2;
    /* lock ext 分栏同步（§6）：读回 ext[] 写 mod.lock */
    if (lock_save(proj_dir, err, errcap) != 0) return 2;
    return 0;
}

int tphp_pkg_ext_add(const char *proj_dir, const char *source, const char *ref,
                     char *err, size_t errcap) {
    if (!source || !source[0]) {
        set_err(err, errcap, "ext --add requires a source (URL, zip, or path)");
        return 1;
    }
    bool is_proj = false;
    int rc = ext_proj_mode(proj_dir, &is_proj, err, errcap);
    if (rc != 0) return rc;
    ExtInfo info;
    rc = ext_install(source, ref, NULL, is_proj ? proj_dir : NULL, false,
                     &info, err, errcap);
    if (rc != 0) return rc;
    if (is_proj) {
        rc = ext_writeback(proj_dir, &info, err, errcap);
        if (rc != 0) return rc;
    }
    /* 2026-11-12 拍板：add 恒编译重链——不再看 .built「已链入」跳过。
       改了源码再 add 立刻出新 tphp（重复 add 的代价 = 一次 relink，秒级）；
       Windows 换像机制不变：本次进程仍跑旧镜像，下次启动即新 tphp。 */
    return ext_compile_all(err, errcap);
}

/* ext[] [lo,hi) 落盘（sync/update 共用）：已装且非 force → skip 行；
 * 否则按 url/ref 安装（url 相对 proj 解析，声明名对账）。逐项结果进 sb。
 * *out_need_link：存在「新装/更新/有源未链入」任一 → 调用方随后 compile_all。 */
static int ext_fill_range(const char *proj_dir, const TMod *root,
                          const char *eroot, int lo, int hi, bool force,
                          bool *out_need_link, PkgBuf *sb,
                          char *err, size_t errcap) {
    if (out_need_link) *out_need_link = false;
    for (int i = lo; i < hi; i++) {
        const ModDep *d = &root->exts[i];
        char dest[1400], dmj[1450];
        snprintf(dest, sizeof dest, "%s/ext/%s", eroot, d->name);
        snprintf(dmj, sizeof dmj, "%s/mod.json", dest);
        bool existed = fs_exists(dmj);
        if (existed && !force) {
            pb_puts(sb, d->name);
            pb_puts(sb, ": already present\n");
            if (out_need_link && !ext_built_has(eroot, d->name))
                *out_need_link = true;
            continue;
        }
        if (!d->url || !d->url[0]) {
            set_err(err, errcap, "ext %s: no url in project ext[]", d->name);
            return 1;
        }
        char absurl[1400], e2[512] = "";
        ext_abs_url(proj_dir, d->url, absurl, sizeof absurl);
        ExtInfo info;
        int irc = ext_install(absurl, d->ref, d->name, proj_dir, force,
                              &info, e2, sizeof e2);
        if (irc != 0) {
            set_err(err, errcap, "ext %s: %s", d->name,
                    e2[0] ? e2 : "install failed");
            return irc;
        }
        pb_puts(sb, d->name);
        pb_puts(sb, (force && existed) ? ": updated\n" : ": installed\n");
        if (out_need_link) *out_need_link = true;
    }
    /* 项目侧 lock ext 分栏（§6）：有变更才写；调用方在 compile 前后均可 */
    if (proj_dir && proj_dir[0] && out_need_link && *out_need_link) {
        char e2[512] = "";
        if (lock_save(proj_dir, e2, sizeof e2) != 0)
            tphp_warn("PHP Warning: pkg: lock_save after ext: %s\n", e2);
    }
    return 0;
}

int tphp_pkg_ext_sync(const char *proj_dir, char **out_summary,
                      char *err, size_t errcap) {
    if (out_summary) *out_summary = NULL;
    bool is_proj = false;
    int rc = ext_proj_mode(proj_dir, &is_proj, err, errcap);
    if (rc != 0) return rc;
    if (!is_proj) {
        set_err(err, errcap,
                "no project mod.json in %s (ext sync needs ext[] declarations)",
                proj_dir ? proj_dir : ".");
        return 1;
    }
    char mj[1250], eroot[1024];
    snprintf(mj, sizeof mj, "%s/mod.json", proj_dir);
    if (!tphp_pkg_ext_root(eroot, sizeof eroot)) {
        set_err(err, errcap, "cannot locate tphp root (self exe unknown)");
        return 2;
    }
    TMod root;
    if (!tmod_load(mj, &root, err, errcap)) return 2;

    PkgBuf sb;
    pb_init(&sb);
    bool need_link = false;
    rc = ext_fill_range(proj_dir, &root, eroot, 0, root.n_exts, false,
                        &need_link, &sb, err, errcap);
    tmod_free(&root);
    if (rc != 0) { tm_free(sb.p); return rc; }
    if (need_link) {
        rc = ext_compile_all(err, errcap);
        if (rc != 0) { tm_free(sb.p); return rc; }
    }
    if (sb.p == NULL) pb_put(&sb, "", 0);
    if (out_summary) *out_summary = sb.p; else tm_free(sb.p);
    return 0;
}

int tphp_pkg_ext_update(const char *proj_dir, const char *name,
                        char **out_summary, char *err, size_t errcap) {
    if (out_summary) *out_summary = NULL;
    bool is_proj = false;
    int rc = ext_proj_mode(proj_dir, &is_proj, err, errcap);
    if (rc != 0) return rc;
    if (!is_proj) {
        set_err(err, errcap,
                "ext --update needs project ext[] (url/ref declarations)");
        return 1;
    }
    char mj[1250], eroot[1024];
    snprintf(mj, sizeof mj, "%s/mod.json", proj_dir);
    if (!tphp_pkg_ext_root(eroot, sizeof eroot)) {
        set_err(err, errcap, "cannot locate tphp root (self exe unknown)");
        return 2;
    }
    TMod root;
    if (!tmod_load(mj, &root, err, errcap)) return 2;

    int lo = 0, hi = root.n_exts;
    if (name) {
        lo = -1;
        for (int i = 0; i < root.n_exts; i++) {
            if (strcmp(root.exts[i].name, name) == 0) { lo = i; break; }
        }
        if (lo < 0) {
            set_err(err, errcap, "ext %s not declared in project ext[]", name);
            tmod_free(&root);
            return 1;
        }
        hi = lo + 1;
    }

    PkgBuf sb;
    pb_init(&sb);
    bool need_link = false;
    rc = ext_fill_range(proj_dir, &root, eroot, lo, hi, true,
                        &need_link, &sb, err, errcap);
    tmod_free(&root);
    if (rc != 0) { tm_free(sb.p); return rc; }
    if (need_link) {
        rc = ext_compile_all(err, errcap);
        if (rc != 0) { tm_free(sb.p); return rc; }
    }
    if (sb.p == NULL) pb_put(&sb, "", 0);
    if (out_summary) *out_summary = sb.p; else tm_free(sb.p);
    return 0;
}

int tphp_pkg_ext_remove(const char *proj_dir, const char *name,
                        char *err, size_t errcap) {
    const char *why = NULL;
    if (!tphp_pkg_valid_name(name, true, &why)) {
        set_err(err, errcap, "invalid ext name \"%s\": %s (need vendor/package)",
                name ? name : "", why ? why : "");
        return 1;
    }
    bool is_proj = false;
    int rc = ext_proj_mode(proj_dir, &is_proj, err, errcap);
    if (rc != 0) return rc;

    char dest[1400];
    if (!ext_pkg_dir(name, dest, sizeof dest)) {
        set_err(err, errcap, "cannot locate tphp root (self exe unknown)");
        return 2;
    }
    if (!fs_exists(dest)) {
        set_err(err, errcap, "ext not installed: %s", name);
        return 1;
    }
    char mj[1450];
    snprintf(mj, sizeof mj, "%s/mod.json", dest);
    if (!fs_exists(mj)) {
        set_err(err, errcap,
                "refusing to delete %s: no mod.json (not an installed ext; "
                "delete it manually if sure)", dest);
        return 2;
    }
    TMod m;
    if (!tmod_load(mj, &m, err, errcap)) return 2;
    bool mismatch = strcmp(m.name, name) != 0;
    tmod_free(&m);
    if (mismatch) {
        set_err(err, errcap,
                "refusing to delete %s: holds a different package name", dest);
        return 2;
    }
    fs_rm_rf(dest);

    if (is_proj) {
        char pmj[1250];
        snprintf(pmj, sizeof pmj, "%s/mod.json", proj_dir);
        TMod root;
        if (!tmod_load(pmj, &root, err, errcap)) return 2;
        for (int i = 0; i < root.n_exts; i++) {
            if (strcmp(root.exts[i].name, name) != 0) continue;
            tm_free(root.exts[i].name);
            tm_free(root.exts[i].url);
            tm_free(root.exts[i].ref);
            memmove(&root.exts[i], &root.exts[i + 1],
                    sizeof(ModDep) * (size_t)(root.n_exts - i - 1));
            root.n_exts--;
            break;
        }
        rc = tmod_save(pmj, &root, err, errcap);
        tmod_free(&root);
        if (rc != 0) return 2;
        /* lock ext 分栏同步（§6） */
        if (lock_save(proj_dir, err, errcap) != 0) return 2;
    }
    return 0;
}

/* .built（§4.3）：每行 "<name> <version>"，成功重链后整体重写。 */
static bool ext_built_has(const char *eroot, const char *name) {
    char p[1200];
    snprintf(p, sizeof p, "%s/ext/.built", eroot);
    size_t len = 0;
    char *raw = fs_read(p, &len);
    if (!raw) return false;
    bool hit = false;
    for (char *line = raw; line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t L = strlen(line);
        if (L && line[L - 1] == '\r') line[L - 1] = 0;
        char *sp = strchr(line, ' ');
        if (sp) *sp = 0;
        if (line[0] && strcmp(line, name) == 0) { hit = true; break; }
        line = nl ? nl + 1 : NULL;
    }
    tm_free(raw);
    return hit;
}

/* --list 行：三列 = source（源码在）× built-in（.built）× project（ext[] 声明） */
typedef struct { char *name; bool src, decl; } ExtRow;

static void extrow_set(ExtRow **rows, int *n, int *cap,
                       const char *name, bool is_src, bool is_decl) {
    for (int i = 0; i < *n; i++) {
        if (strcmp((*rows)[i].name, name) != 0) continue;
        (*rows)[i].src |= is_src;
        (*rows)[i].decl |= is_decl;
        return;
    }
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *rows = tm_realloc(*rows, sizeof(ExtRow) * (size_t)*cap);
    }
    (*rows)[*n].name = xstrdup(name);
    (*rows)[*n].src = is_src;
    (*rows)[*n].decl = is_decl;
    (*n)++;
}

/* 扫带 mod.json 的扩展条目：dir 自身（一级布局）或 dir/<vendor>/<pkg>
 *（两级，§1 模型）；'.' 开头跳过（.staging 等）。name 取 mod.json，
 * 读不开用相对路径。 */
static void ext_scan_dir(const char *dir, const char *rel_prefix,
                         ExtRow **rows, int *n, int *cap) {
    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, dir, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        return;
    }
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;
        char sub[1600], rel[512], mj[1620];
        if (rel_prefix[0]) snprintf(rel, sizeof rel, "%s/%s", rel_prefix, ent.name);
        else snprintf(rel, sizeof rel, "%s", ent.name);
        snprintf(sub, sizeof sub, "%s/%s", dir, ent.name);
        if (!fs_isdir(sub)) continue;
        snprintf(mj, sizeof mj, "%s/mod.json", sub);
        if (fs_exists(mj)) {
            TMod m;
            if (tmod_load(mj, &m, NULL, 0)) {
                extrow_set(rows, n, cap, m.name, true, false);
                tmod_free(&m);
            } else {
                extrow_set(rows, n, cap, rel, true, false);
            }
            continue;
        }
        if (rel_prefix[0]) continue;   /* 只扫两级 */
        ext_scan_dir(sub, rel, rows, n, cap);
    }
    uv_fs_req_cleanup(&req);
}

static int extrow_cmp(const void *a, const void *b) {
    return strcmp(((const ExtRow *)a)->name, ((const ExtRow *)b)->name);
}

int tphp_pkg_ext_list(const char *proj_dir, char **out,
                      char *err, size_t errcap) {
    *out = NULL;
    char eroot[1024];
    if (!tphp_pkg_ext_root(eroot, sizeof eroot)) {
        set_err(err, errcap, "cannot locate tphp root (self exe unknown)");
        return 2;
    }
    bool is_proj = false;
    int rc = ext_proj_mode(proj_dir, &is_proj, err, errcap);
    if (rc != 0) return rc;

    ExtRow *rows = NULL;
    int n = 0, cap = 0;
    char extdir[1100];
    snprintf(extdir, sizeof extdir, "%s/ext", eroot);
    ext_scan_dir(extdir, "", &rows, &n, &cap);

    if (is_proj) {
        char mj[1250];
        snprintf(mj, sizeof mj, "%s/mod.json", proj_dir);
        TMod root;
        if (!tmod_load(mj, &root, err, errcap)) {
            for (int i = 0; i < n; i++) tm_free(rows[i].name);
            tm_free(rows);
            return 2;
        }
        for (int i = 0; i < root.n_exts; i++)
            extrow_set(&rows, &n, &cap, root.exts[i].name, false, true);
        tmod_free(&root);
    }

    if (n > 1) qsort(rows, (size_t)n, sizeof(ExtRow), extrow_cmp);

    PkgBuf b;
    pb_init(&b);
    pb_puts(&b, "name                source  built-in  project\n");
    for (int i = 0; i < n; i++) {
        char line[512];
        snprintf(line, sizeof line, "%-20s  %-6s  %-8s  %s\n",
                 rows[i].name,
                 rows[i].src ? "y" : "n",
                 ext_built_has(eroot, rows[i].name) ? "y" : "n",
                 is_proj ? (rows[i].decl ? "y" : "n") : "-");
        pb_puts(&b, line);
        tm_free(rows[i].name);
    }
    tm_free(rows);
    if (b.p == NULL) pb_put(&b, "", 0);
    *out = b.p;
    return 0;
}

/* ================================================================
 * ext 编译 + 链接（PLAN/ext-packages.md §5，切片 5）
 * ================================================================ */

/* 运行期 init 钩子：基座在 libtphp.a；重链时生成的 ext_registry.o
 * 用 constructor 把自己的 run_all 挂上（避免 weak/重复符号）。 */
void (*tphp_ext_inits_hook)(void) = 0;
void tphp_run_ext_inits(void) {
#ifdef _WIN32
    /* relink 遗留 tphp.exe.old：尽力删（仍被占用则下次启动再试） */
    if (g_self_exe[0]) {
        char oldp[1200];
        snprintf(oldp, sizeof oldp, "%s.old", g_self_exe);
        remove(oldp);
    }
#endif
    if (tphp_ext_inits_hook) tphp_ext_inits_hook();
}

/* ---- 用户 ext 注册 API v1（PLAN/ext-packages.md §2.3；init 内调用）----
 * 生命周期 = 进程（tphp_run_ext_inits 只在首个脚本前跑一次，注册先于
 * 编译/执行）。查询口在 opcode.h（vm.c resolve_fn / op_fetch_const 挂钩，
 * 另 bi_defined 认常量）。类无需注册 API：include/class.h 直接按 exclass
 * 蓝本构造 TClass + tphp_class_register_compile() 即可。 */
typedef struct { const char *name; TVal v; } ExtConstN;
typedef struct { const char *name; BuiltinFn fn; } ExtFnN;
static ExtConstN *s_ext_consts; static int s_n_ext_consts;
static ExtFnN *s_ext_fns;       static int s_n_ext_fns;

void tphp_ext_reg_const_str(const char *fq_name, const char *v) {
    if (!fq_name || !v) return;
    s_ext_consts = realloc(s_ext_consts,
                           sizeof(ExtConstN) * (size_t)(s_n_ext_consts + 1));
    s_ext_consts[s_n_ext_consts].name = tstr_val(tstr_intern(fq_name, strlen(fq_name)));
    s_ext_consts[s_n_ext_consts].v = TV_STR(tstr_new(v, strlen(v)));
    s_n_ext_consts++;
}

void tphp_ext_reg_const_int(const char *fq_name, int64_t v) {
    if (!fq_name) return;
    s_ext_consts = realloc(s_ext_consts,
                           sizeof(ExtConstN) * (size_t)(s_n_ext_consts + 1));
    s_ext_consts[s_n_ext_consts].name = tstr_val(tstr_intern(fq_name, strlen(fq_name)));
    s_ext_consts[s_n_ext_consts].v = TV_INT(v);
    s_n_ext_consts++;
}

void tphp_ext_reg_fn(const char *fq_name, BuiltinFn fn) {
    if (!fq_name || !fn) return;
    s_ext_fns = realloc(s_ext_fns, sizeof(ExtFnN) * (size_t)(s_n_ext_fns + 1));
    s_ext_fns[s_n_ext_fns].name = tstr_val(tstr_intern(fq_name, strlen(fq_name)));
    s_ext_fns[s_n_ext_fns].fn = fn;
    s_n_ext_fns++;
}

/* vm.c 查询口：命中返回 addref 后的 TVal / 原生 fn 指针 */
bool tphp_ext_const_find(const char *name, TVal *out) {
    for (int i = 0; i < s_n_ext_consts; i++)
        if (strcmp(s_ext_consts[i].name, name) == 0) {
            *out = s_ext_consts[i].v;
            tval_addref(*out);
            return true;
        }
    return false;
}

void *tphp_ext_fn_find(const char *name) {
    for (int i = 0; i < s_n_ext_fns; i++)
        if (strcmp(s_ext_fns[i].name, name) == 0)
            return (void *)s_ext_fns[i].fn;
    return 0;
}

static int ext_current_os(void) {
#if defined(_WIN32)
    return EXT_OS_WIN;
#elif defined(__APPLE__)
    return EXT_OS_DARWIN;
#else
    return EXT_OS_LINUX;
#endif
}

static int ext_sys_run(const char *cmd) {
#ifdef _WIN32
    int rc = system(cmd);
    return rc == -1 ? -1 : (rc & 0xFF);
#else
    int rc = system(cmd);
    if (rc == -1) return -1;
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
#endif
}

/* 引擎级默认链接库（§5：Windows 对齐 build.cmd；unix 见 build.sh LDLIBS） */
static const char *ext_engine_libs(int os) {
    if (os == EXT_OS_WIN)
        return "-ldbghelp -lws2_32 -ladvapi32 -luser32 -liphlpapi -lpsapi "
               "-lshell32 -lole32 -loleaut32 -luuid -lbcrypt "
               "-lsynchronization -luserenv -lcrypt32";
    if (os == EXT_OS_DARWIN)
        return "-lm -framework CoreFoundation -framework CoreServices";
    return "-lpthread -ldl -lrt -lm";
}

/* cc 探测：os.cc > TPHP_EXT_CC > <root>/tcc > clang > cc/gcc */
static bool ext_find_cc(const char *eroot, const TExtSpec *spec,
                        char *out, size_t cap) {
    int os = ext_current_os();
    if (spec && spec->os_cc[os] && spec->os_cc[os][0]) {
        snprintf(out, cap, "%s", spec->os_cc[os]);
        return true;
    }
    const char *env = getenv("TPHP_EXT_CC");
    if (env && env[0]) {
        snprintf(out, cap, "%s", env);
        return true;
    }
#ifdef _WIN32
    snprintf(out, cap, "%s/tcc/tcc.exe", eroot);
    if (fs_exists(out)) return true;
    snprintf(out, cap, "%s/tcc/tcc", eroot);
    if (fs_exists(out)) return true;
    snprintf(out, cap, "clang");
    return true;   /* PATH 探测交给 system */
#else
    snprintf(out, cap, "%s/tcc/tcc", eroot);
    if (fs_exists(out)) return true;
    snprintf(out, cap, "clang");
    return true;
#endif
}

/* 递归收集 *.c（跳过 build/ 与 '.' 开头条目） */
static int ext_collect_cs(const char *dir, char ***out, int *n) {
    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, dir, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        return -1;
    }
    uv_dirent_t ent;
    int rc = 0;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;
        char path[1600];
        snprintf(path, sizeof path, "%s/%s", dir, ent.name);
        if (fs_isdir(path)) {
            if (strcmp(ent.name, "build") == 0) continue;
            if (ext_collect_cs(path, out, n) != 0) { rc = -1; break; }
        } else {
            size_t L = strlen(ent.name);
            if (L > 2 && strcmp(ent.name + L - 2, ".c") == 0)
                strlist_push(out, n, path);
        }
    }
    uv_fs_req_cleanup(&req);
    return rc;
}

/* 扫 <eroot>/ext 下 type=ext 包目录（一级 + 两级），返回目录列表 */
static int ext_find_pkg_dirs(const char *eroot, char ***out, int *n) {
    char base[1200];
    snprintf(base, sizeof base, "%s/ext", eroot);
    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, base, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        return 0;
    }
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;
        char sub[1400];
        snprintf(sub, sizeof sub, "%s/%s", base, ent.name);
        if (!fs_isdir(sub)) continue;
        char mj[1450];
        snprintf(mj, sizeof mj, "%s/mod.json", sub);
        if (fs_exists(mj)) {
            strlist_push(out, n, sub);
            continue;
        }
        /* 两级：vendor/pkg */
        uv_fs_t r2;
        if (uv_fs_scandir(NULL, &r2, sub, 0, NULL) < 0) {
            uv_fs_req_cleanup(&r2);
            continue;
        }
        uv_dirent_t e2;
        while (uv_fs_scandir_next(&r2, &e2) == 0) {
            if (e2.name[0] == '.') continue;
            char pkg[1600], pmj[1650];
            snprintf(pkg, sizeof pkg, "%s/%s", sub, e2.name);
            if (!fs_isdir(pkg)) continue;
            snprintf(pmj, sizeof pmj, "%s/mod.json", pkg);
            if (fs_exists(pmj)) strlist_push(out, n, pkg);
        }
        uv_fs_req_cleanup(&r2);
    }
    uv_fs_req_cleanup(&req);
    return 0;
}

/* 拼 -I/-D/cflags：包 include + 根 include；default cflags + os cflags */
static void ext_append_compile_args(PkgBuf *b, const char *eroot,
                                    const char *pkgdir, const TExtSpec *x) {
    int os = ext_current_os();
    pb_puts(b, " -I\"");
    pb_puts(b, eroot);
    pb_puts(b, "/include\"");
    for (int i = 0; i < x->n_include; i++) {
        pb_puts(b, " -I\"");
        pb_puts(b, pkgdir);
        pb_puts(b, "/");
        pb_puts(b, x->include[i]);
        pb_puts(b, "\"");
    }
    for (int i = 0; i < x->n_defines; i++) {
        pb_puts(b, " -D");
        pb_puts(b, x->defines[i]);
    }
    for (int i = 0; i < x->n_cflags; i++) {
        pb_puts(b, " ");
        pb_puts(b, x->cflags[i]);
    }
    for (int i = 0; i < x->n_os_cflags[os]; i++) {
        pb_puts(b, " ");
        pb_puts(b, x->os_cflags[os][i]);
    }
}

/* 编译单个包的全部 .c → objs[]（调用方提供缓冲）；返回 0 成功 */
static int ext_compile_pkg(const char *eroot, const char *pkgdir,
                           const TMod *m, const char *cc,
                           char ***objs, int *nobjs,
                           char *err, size_t errcap) {
    const TExtSpec *x = &m->ext_spec;
    if (x->n_sources == 0) {
        set_err(err, errcap, "%s: ext.sources is empty", m->name);
        return 1;
    }
    if (x->tphp_abi && x->tphp_abi[0] &&
        strcmp(x->tphp_abi, TPHP_VERSION) != 0) {
        set_err(err, errcap,
                "%s: ABI mismatch (package requires tphp %s, this is %s)",
                m->name, x->tphp_abi, TPHP_VERSION);
        return 1;
    }

    char **cs = NULL;
    int ncs = 0;
    for (int i = 0; i < x->n_sources; i++) {
        char sdir[1500];
        snprintf(sdir, sizeof sdir, "%s/%s", pkgdir, x->sources[i]);
        if (!fs_isdir(sdir)) {
            set_err(err, errcap, "%s: sources dir not found: %s",
                    m->name, x->sources[i]);
            tm_strlist_free(cs, ncs);
            return 1;
        }
        if (ext_collect_cs(sdir, &cs, &ncs) != 0) {
            set_err(err, errcap, "%s: cannot scan %s", m->name, x->sources[i]);
            tm_strlist_free(cs, ncs);
            return 2;
        }
    }
    if (ncs == 0) {
        set_err(err, errcap, "%s: no .c files under sources", m->name);
        tm_strlist_free(cs, ncs);
        return 1;
    }

    char bdir[1500];
    snprintf(bdir, sizeof bdir, "%s/build", pkgdir);
    if (fs_mkdir_p(bdir) < 0) {
        set_err(err, errcap, "%s: cannot create build/", m->name);
        tm_strlist_free(cs, ncs);
        return 2;
    }

    char flags[4096];
    {
        PkgBuf fb;
        pb_init(&fb);
        ext_append_compile_args(&fb, eroot, pkgdir, x);
        snprintf(flags, sizeof flags, "%s", fb.p ? fb.p : "");
        tm_free(fb.p);
    }

    for (int i = 0; i < ncs; i++) {
        /* 对象名：相对 pkgdir 打平 src_foo.o */
        const char *rel = cs[i];
        size_t plen = strlen(pkgdir);
        if (strncmp(rel, pkgdir, plen) == 0) {
            rel += plen;
            while (*rel == '/' || *rel == '\\') rel++;
        }
        char flat[512];
        size_t k = 0;
        for (const char *p = rel; *p && k + 4 < sizeof flat; p++) {
            char c = *p;
            if (c == '/' || c == '\\' || c == '.' || c == ':') c = '_';
            flat[k++] = c;
        }
        flat[k] = 0;
        char obj[1600], cmd[8192];
        snprintf(obj, sizeof obj, "%s/%s.o", bdir, flat);
        snprintf(cmd, sizeof cmd, "%s -c%s -o \"%s\" \"%s\"",
                 cc, flags, obj, cs[i]);
        int rc = ext_sys_run(cmd);
        if (rc != 0) {
            set_err(err, errcap, "%s: compile failed (%d): %s",
                    m->name, rc, cs[i]);
            tm_strlist_free(cs, ncs);
            return 2;
        }
        strlist_push(objs, nobjs, obj);
    }
    tm_strlist_free(cs, ncs);
    return 0;
}

/* 生成 <eroot>/ext/.registry.c：constructor 挂 tphp_ext_inits_hook */
static int ext_write_registry(const char *eroot,
                              char **pkgs, int npkg, char **inits,
                              char *err, size_t errcap) {
    PkgBuf b;
    pb_init(&b);
    pb_puts(&b, "/* generated by tphp ext relink — do not edit */\n");
    pb_puts(&b, "extern void (*tphp_ext_inits_hook)(void);\n");
    for (int i = 0; i < npkg; i++) {
        pb_puts(&b, "void ");
        pb_puts(&b, inits[i]);
        pb_puts(&b, "(void);\n");
    }
    pb_puts(&b, "static void tphp_ext_run_all(void) {\n");
    for (int i = 0; i < npkg; i++) {
        pb_puts(&b, "    ");
        pb_puts(&b, inits[i]);
        pb_puts(&b, "();\n");
    }
    pb_puts(&b, "}\n");
    pb_puts(&b, "__attribute__((constructor)) static void tphp_ext_install_hook(void) {\n");
    pb_puts(&b, "    tphp_ext_inits_hook = tphp_ext_run_all;\n");
    pb_puts(&b, "}\n");
    (void)pkgs;

    char path[1400];
    snprintf(path, sizeof path, "%s/ext/.registry.c", eroot);
    int rc = fs_write(path, b.p, b.len);
    tm_free(b.p);
    if (rc < 0) {
        set_err(err, errcap, "cannot write %s", path);
        return 2;
    }
    return 0;
}

/* 路径归一为 /（clang @rsp 在 Windows 会把 \ 当转义） */
static void ext_path_fwd(char *s) {
    for (char *q = s; *q; q++) if (*q == '\\') *q = '/';
}

/* rsp 逐 token 独立成行（clang @rsp 一行一项） */
static void rsp_put_tokens(PkgBuf *lb, const char *s) {
    if (!s) return;
    const char *p = s;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *t = p;
        while (*p && *p != ' ') p++;
        pb_put(lb, t, (size_t)(p - t));
        pb_put(lb, "\n", 1);
    }
}

/* Windows 8191 上限 → @response 文件（clang/lld 支持；路径用 /）。
 * extra_link：包契约声明的链接指令（libs + os.libs + ldflags + os.ldflags，
 * 空格分隔），追加在引擎库之后、-o 之前。 */
static int ext_write_link_rsp(const char *rsp, const char *reg_o,
                              char **objs, int nobjs,
                              char **eng, int neng, const char *next,
                              const char *extra_link,
                              char *err, size_t errcap) {
    PkgBuf lb;
    pb_init(&lb);
    char tmp[1600];
    snprintf(tmp, sizeof tmp, "%s", reg_o); ext_path_fwd(tmp);
    pb_puts(&lb, "\""); pb_puts(&lb, tmp); pb_puts(&lb, "\"\n");
    for (int i = 0; i < nobjs; i++) {
        snprintf(tmp, sizeof tmp, "%s", objs[i]); ext_path_fwd(tmp);
        pb_puts(&lb, "\""); pb_puts(&lb, tmp); pb_puts(&lb, "\"\n");
    }
    for (int i = 0; i < neng; i++) {
        snprintf(tmp, sizeof tmp, "%s", eng[i]); ext_path_fwd(tmp);
        pb_puts(&lb, "\""); pb_puts(&lb, tmp); pb_puts(&lb, "\"\n");
    }
    rsp_put_tokens(&lb, ext_engine_libs(ext_current_os()));
    if (extra_link && extra_link[0]) rsp_put_tokens(&lb, extra_link);
    pb_puts(&lb, "-o\n\"");
    snprintf(tmp, sizeof tmp, "%s", next); ext_path_fwd(tmp);
    pb_puts(&lb, tmp);
    pb_puts(&lb, "\"\n");
    int rc = fs_write(rsp, lb.p, lb.len);
    tm_free(lb.p);
    if (rc < 0) {
        set_err(err, errcap, "cannot write %s", rsp);
        return 2;
    }
    return 0;
}

/* 链接 all .o + registry.o + 引擎对象 → tphp.next，再原子替换。
 * 引擎对象：优先 <root>/obj/*.o（与 build.cmd 同款直链，避开
 * libtphp.a 的 pcre2 dllimport 归档语义）；否则解压 libtphp.a。 */
static int ext_link_and_replace(const char *eroot, const char *cc,
                                char **objs, int nobjs, const char *extra_link,
                                char *err, size_t errcap) {
    char liba[1400], next[1400], reg_c[1400], reg_o[1400];
    snprintf(liba, sizeof liba, "%s/libtphp.a", eroot);
    snprintf(reg_c, sizeof reg_c, "%s/ext/.registry.c", eroot);
    snprintf(reg_o, sizeof reg_o, "%s/ext/.registry.o", eroot);
    if (!fs_exists(reg_c)) {
        set_err(err, errcap, "registry source missing: %s", reg_c);
        return 2;
    }
    {
        char cmd[4096], rcpath[1600], rcsrc[1600];
        snprintf(rcpath, sizeof rcpath, "%s", reg_o); ext_path_fwd(rcpath);
        snprintf(rcsrc, sizeof rcsrc, "%s", reg_c); ext_path_fwd(rcsrc);
        snprintf(cmd, sizeof cmd, "%s -c -o \"%s\" \"%s\"", cc, rcpath, rcsrc);
        int rc = ext_sys_run(cmd);
        if (rc != 0) {
            set_err(err, errcap, "compile registry failed (%d)", rc);
            return 2;
        }
    }

    char **eng = NULL;
    int neng = 0;
    char linkdir[1400];
    linkdir[0] = 0;
    char objdir[1400];
    snprintf(objdir, sizeof objdir, "%s/obj", eroot);
    if (fs_isdir(objdir)) {
        uv_fs_t req;
        if (uv_fs_scandir(NULL, &req, objdir, 0, NULL) >= 0) {
            uv_dirent_t ent;
            while (uv_fs_scandir_next(&req, &ent) == 0) {
                size_t L = strlen(ent.name);
                if (L > 2 && strcmp(ent.name + L - 2, ".o") == 0) {
                    char p[1600];
                    snprintf(p, sizeof p, "%s/%s", objdir, ent.name);
                    strlist_push(&eng, &neng, p);
                }
            }
            uv_fs_req_cleanup(&req);
        }
    }
    if (neng == 0) {
        if (!fs_exists(liba)) {
            set_err(err, errcap,
                    "libtphp.a not found in %s (need full artifact: tphp + "
                    "libtphp.a + include/)", eroot);
            return 2;
        }
        snprintf(linkdir, sizeof linkdir, "%s/ext/.linkobjs", eroot);
        fs_rm_rf(linkdir);
        if (fs_mkdir_p(linkdir) < 0) {
            set_err(err, errcap, "cannot create %s", linkdir);
            return 2;
        }
        char cmd[2048], la[1600], ld[1600];
        snprintf(la, sizeof la, "%s", liba); ext_path_fwd(la);
        snprintf(ld, sizeof ld, "%s", linkdir); ext_path_fwd(ld);
#ifdef _WIN32
        snprintf(cmd, sizeof cmd,
                 "cd /d \"%s\" && (llvm-ar x \"%s\" || ar x \"%s\")",
                 ld, la, la);
#else
        snprintf(cmd, sizeof cmd,
                 "cd \"%s\" && (llvm-ar x \"%s\" || ar x \"%s\")",
                 ld, la, la);
#endif
        if (ext_sys_run(cmd) != 0) {
            set_err(err, errcap, "extract %s failed", liba);
            return 2;
        }
        uv_fs_t req;
        if (uv_fs_scandir(NULL, &req, linkdir, 0, NULL) >= 0) {
            uv_dirent_t ent;
            while (uv_fs_scandir_next(&req, &ent) == 0) {
                size_t L = strlen(ent.name);
                if (L > 2 && strcmp(ent.name + L - 2, ".o") == 0) {
                    char p[1600];
                    snprintf(p, sizeof p, "%s/%s", linkdir, ent.name);
                    strlist_push(&eng, &neng, p);
                }
            }
            uv_fs_req_cleanup(&req);
        }
        if (neng == 0) {
            set_err(err, errcap, "no .o members in %s", liba);
            return 2;
        }
    }

    const char *bn = "tphp";
    if (g_self_exe[0]) {
        const char *p = g_self_exe;
        for (const char *q = g_self_exe; *q; q++)
            if (*q == '/' || *q == '\\') p = q + 1;
        if (p[0]) bn = p;
    }
    snprintf(next, sizeof next, "%s/%s.next", eroot, bn);

    char rsp[1400];
    snprintf(rsp, sizeof rsp, "%s/ext/.link.rsp", eroot);
    if (ext_write_link_rsp(rsp, reg_o, objs, nobjs, eng, neng,
                           next, extra_link, err, errcap) != 0) {
        tm_strlist_free(eng, neng);
        return 2;
    }
    char cmd[1600], rspp[1600];
    snprintf(rspp, sizeof rspp, "%s", rsp); ext_path_fwd(rspp);
    snprintf(cmd, sizeof cmd, "%s @\"%s\"", cc, rspp);
    int rc = ext_sys_run(cmd);
    remove(rsp);
    if (rc != 0) {
        set_err(err, errcap, "link failed (%d): %s @%s", rc, cc, rsp);
        tm_strlist_free(eng, neng);
        return 2;
    }
    tm_strlist_free(eng, neng);
    if (linkdir[0]) fs_rm_rf(linkdir);

    /* 原子替换 */
    char cur[1400], oldp[1400];
    snprintf(cur, sizeof cur, "%s/%s", eroot, bn);
    snprintf(oldp, sizeof oldp, "%s/%s.old", eroot, bn);
#ifdef _WIN32
    /* 两次 relink 同进程：第一次 cur=运行中镜像 → rename 到 .old；
     * 第二次 cur=上次产物（未运行）→ 直接 remove 再放入 next。
     * .old 可能仍被旧进程占用，remove 失败可忽略。 */
    remove(oldp);
    if (remove(cur) == 0) {
        if (rename(next, cur) != 0) {
            set_err(err, errcap, "cannot move %s into place", next);
            remove(next);
            return 2;
        }
    } else {
        if (rename(cur, oldp) != 0) {
            set_err(err, errcap, "cannot rename running %s; "
                    "close other tphp processes and retry --relink", cur);
            remove(next);
            return 2;
        }
        if (rename(next, cur) != 0) {
            set_err(err, errcap, "cannot move %s into place", next);
            rename(oldp, cur);
            return 2;
        }
    }
#else
    if (rename(next, cur) != 0) {
        set_err(err, errcap, "cannot replace %s: %s", cur, strerror(errno));
        remove(next);
        return 2;
    }
#endif
    return 0;
}

/* 成功重链后整体重写 ext/.built（§4.3） */
static int ext_rewrite_built(const char *eroot,
                             char **names, char **versions, int n,
                             char *err, size_t errcap) {
    PkgBuf b;
    pb_init(&b);
    for (int i = 0; i < n; i++) {
        pb_puts(&b, names[i]);
        pb_puts(&b, " ");
        pb_puts(&b, versions[i] ? versions[i] : "0.0.0");
        pb_puts(&b, "\n");
    }
    char path[1400];
    snprintf(path, sizeof path, "%s/ext/.built", eroot);
    int rc = fs_write(path, b.p ? b.p : "", b.len);
    tm_free(b.p);
    if (rc < 0) {
        set_err(err, errcap, "cannot write %s", path);
        return 2;
    }
    return 0;
}

/* 全量：编译所有 type=ext 包 → registry → 链接替换 → .built */
static int ext_compile_all(char *err, size_t errcap) {
    char eroot[1024];
    if (!tphp_pkg_ext_root(eroot, sizeof eroot)) {
        set_err(err, errcap, "cannot locate tphp root (self exe unknown)");
        return 2;
    }
    char **dirs = NULL;
    int ndirs = 0;
    ext_find_pkg_dirs(eroot, &dirs, &ndirs);
    if (ndirs == 0) {
        tm_strlist_free(dirs, ndirs);
        set_err(err, errcap, "no ext packages under %s/ext", eroot);
        return 1;
    }

    char cc[512];
    /* 首个包的契约参与 cc 探测（os.cc）；失败则默认链 */
    {
        char mj0[1600];
        snprintf(mj0, sizeof mj0, "%s/mod.json", dirs[0]);
        TMod m0;
        if (tmod_load(mj0, &m0, NULL, 0)) {
            ext_find_cc(eroot, &m0.ext_spec, cc, sizeof cc);
            tmod_free(&m0);
        } else {
            ext_find_cc(eroot, NULL, cc, sizeof cc);
        }
    }

    char **objs = NULL;
    int nobjs = 0;
    char **names = NULL;
    char **versions = NULL;
    char **inits = NULL;
    int nbuild = 0;
    int rc = 0;
    /* 包契约声明的链接指令（libs/os.libs/ldflags/os.ldflags）累积，
     * 链接时随 rsp 一并下发（§2 契约字段的消费端） */
    PkgBuf xlink;
    pb_init(&xlink);
    int os_now = ext_current_os();

    for (int i = 0; i < ndirs; i++) {
        char mj[1650];
        snprintf(mj, sizeof mj, "%s/mod.json", dirs[i]);
        TMod m;
        if (!tmod_load(mj, &m, err, errcap)) { rc = 2; break; }
        if (!m.has_ext_spec || !m.type || strcmp(m.type, "ext") != 0) {
            set_err(err, errcap, "%s: not type=ext", dirs[i]);
            tmod_free(&m);
            rc = 1;
            break;
        }
        char pkgcc[512];
        ext_find_cc(eroot, &m.ext_spec, pkgcc, sizeof pkgcc);
        /* 单包编译用包自己的 cc；链接统一用首个探测结果（见上） */
        int crc = ext_compile_pkg(eroot, dirs[i], &m, pkgcc,
                                  &objs, &nobjs, err, errcap);
        if (crc != 0) { tmod_free(&m); rc = crc; break; }

        char sym[600];
        if (m.ext_spec.init && m.ext_spec.init[0])
            snprintf(sym, sizeof sym, "%s", m.ext_spec.init);
        else
            ext_init_sym(m.name, sym, sizeof sym);

        names = tm_realloc(names, sizeof(char *) * (size_t)(nbuild + 1));
        versions = tm_realloc(versions, sizeof(char *) * (size_t)(nbuild + 1));
        inits = tm_realloc(inits, sizeof(char *) * (size_t)(nbuild + 1));
        names[nbuild] = xstrdup(m.name);
        versions[nbuild] = xstrdup(m.version ? m.version : "0.0.0");
        inits[nbuild] = xstrdup(sym);
        nbuild++;
        {
            const TExtSpec *xs = &m.ext_spec;
            struct { char **a; int n; } gl[] = {
                { xs->libs, xs->n_libs },
                { xs->ldflags, xs->n_ldflags },
                { xs->os_libs[os_now], xs->n_os_libs[os_now] },
                { xs->os_ldflags[os_now], xs->n_os_ldflags[os_now] },
            };
            for (size_t g = 0; g < sizeof gl / sizeof gl[0]; g++)
                for (int j = 0; j < gl[g].n; j++) {
                    pb_put(&xlink, " ", 1);
                    pb_puts(&xlink, gl[g].a[j]);
                }
        }
        tmod_free(&m);
    }

    if (rc == 0) {
        rc = ext_write_registry(eroot, names, nbuild, inits, err, errcap);
    }
    if (rc == 0) {
        rc = ext_link_and_replace(eroot, cc, objs, nobjs, xlink.p,
                                  err, errcap);
    }
    if (rc == 0) {
        rc = ext_rewrite_built(eroot, names, versions, nbuild, err, errcap);
    }
    if (rc == 0) {
        /* stderr：不污染 EXPECT（stdout）对拍 */
        fprintf(stderr, "Linked %d ext package(s) into tphp\n", nbuild);
    }

    for (int i = 0; i < nbuild; i++) {
        tm_free(names[i]); tm_free(versions[i]); tm_free(inits[i]);
    }
    tm_free(names); tm_free(versions); tm_free(inits);
    tm_free(xlink.p);
    tm_strlist_free(objs, nobjs);
    tm_strlist_free(dirs, ndirs);
    return rc;
}

int tphp_pkg_ext_relink(char *err, size_t errcap) {
    return ext_compile_all(err, errcap);
}

void tphp_pkg_free(void *p) { tm_free(p); }
