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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>
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

typedef struct {
    char *name, *version, *type, *author, *description, *license;
    ModPsr4 *psr4; int n_psr4;
    char **files; int n_files;
    char **pack_include; int n_include;
    char **pack_exclude; int n_exclude;
    ModDep *deps; int n_deps;
} TMod;

static void tmod_init(TMod *m) { memset(m, 0, sizeof *m); }

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
    pb_puts(&b, "\n}\n");
    int rc = fs_write(path, b.p, b.len);
    tm_free(b.p);
    if (rc < 0) set_err(err, errcap, "cannot write %s", path);
    return rc < 0 ? 2 : 0;
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
        pb_puts(&rb, is_lib ? "\n\n`tphp` library package.\n"
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
 * 根、'/' 分隔。返回 0；scandir 失败 -1。 */
static int integrity_scan(const char *dir, const char *prefix, IntList *l) {
    uv_fs_t req;
    if (uv_fs_scandir(NULL, &req, dir, 0, NULL) < 0) {
        uv_fs_req_cleanup(&req);
        return -1;
    }
    uv_dirent_t ent;
    while (uv_fs_scandir_next(&req, &ent) == 0) {
        if (ent.name[0] == '.') continue;
        char sub[1500], rel[1024];
        snprintf(sub, sizeof sub, "%s/%s", dir, ent.name);
        if (prefix[0]) snprintf(rel, sizeof rel, "%s/%s", prefix, ent.name);
        else snprintf(rel, sizeof rel, "%s", ent.name);
        if (fs_isdir(sub)) {
            if (integrity_scan(sub, rel, l) != 0) {
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
 * snap=NULL → 不校验（旧 lock 向后兼容）。0 通过；-1 不匹配（err 列出）。 */
static int integrity_check(const char *vdir, TArr *snap,
                           char *err, size_t errcap) {
    if (!snap) return 0;
    IntList expect = {0}, actual = {0};
    integrity_from_snap(snap, &expect);
    if (integrity_scan(vdir, "", &actual) != 0) {
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
        pb_puts(&b, (i + 1 < n) ? ",\n" : "\n");
        tm_free(names[i]); tm_free(urls[i]); tm_free(refs[i]);
    }
    tm_free(names); tm_free(urls); tm_free(refs);
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

    int rc = install_deps(proj_dir, &pkg, err, errcap);
    tmod_free(&pkg);
    return rc;
}

static int pkg_install_core(const char *proj_dir, const char *source,
                            const char *ref, char **out_name,
                            char *out_resolved_ref,
                            char *err, size_t errcap) {
    char staging[1250];
    snprintf(staging, sizeof staging, "%s/.pkg-staging", proj_dir);
    fs_rm_rf(staging);

    int rc;
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
                    fprintf(stderr, "PHP Warning: pkg: cannot resolve "
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
        rc = g_fetch(to_get, &buf, &blen, err, errcap);
        if (rc != 0) { fs_rm_rf(staging); return rc; }
        /* zip 魔数是 PK；以 '<' 开头只会是 HTML/JSON 等错误页 */
        if (blen > 0 && buf[0] == '<') {
            set_err(err, errcap,
                    "download of %s is not a zip (got an HTML page?)",
                    to_get);
            free(buf);
            fs_rm_rf(staging);
            return 2;
        }
        char zdir[1300];
        snprintf(zdir, sizeof zdir, "%s/zip", staging);
        rc = zip_extract_mem(buf, blen, zdir, err, errcap);
        free(buf);
        if (rc != 0) { fs_rm_rf(staging); return rc; }
        char root[1300];
        if (find_pkg_root(zdir, root, sizeof root) != 0) {
            set_err(err, errcap, "no mod.json found in downloaded package");
            fs_rm_rf(staging);
            return 2;
        }
        rc = install_from_src(proj_dir, root, out_name, err, errcap);
        fs_rm_rf(staging);
        return rc;
    }

    if (fs_isdir(source)) {
        char mj[1300];
        snprintf(mj, sizeof mj, "%s/mod.json", source);
        if (!fs_exists(mj)) {
            set_err(err, errcap, "no mod.json in %s", source);
            return 1;
        }
        return install_from_src(proj_dir, source, out_name, err, errcap);
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
        rc = zip_extract_mem(zbuf, zlen, zdir, err, errcap);
        tm_free(zbuf);
        if (rc != 0) { fs_rm_rf(staging); return rc; }
        char root[1300];
        if (find_pkg_root(zdir, root, sizeof root) != 0) {
            set_err(err, errcap, "no mod.json found in zip %s", source);
            fs_rm_rf(staging);
            return 2;
        }
        rc = install_from_src(proj_dir, root, out_name, err, errcap);
        fs_rm_rf(staging);
        return rc;
    }

    set_err(err, errcap, "source not found: %s", source);
    return 1;
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

    char *pkg_name = NULL;
    int rc = pkg_install_core(proj_dir, source, eff, &pkg_name, resolved,
                              err, errcap);
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
    tmod_free(&root);

    lock_save(proj_dir, err, errcap);
    tphp_pkg_gen_autoload(proj_dir, err, errcap);
    return 0;
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
    tmod_free(&root);
    lock_save(proj_dir, err, errcap);
    tphp_pkg_gen_autoload(proj_dir, err, errcap);
    return 0;
}

/* ---- install（按 mod.lock 恢复；无 lock 则从 mod.json deps 全量） ---- */

int tphp_pkg_install(const char *proj_dir, bool force,
                     char *err, size_t errcap) {
    if (!proj_dir || !proj_dir[0]) {
        set_err(err, errcap, "install requires project dir");
        return 1;
    }
    char root_mj[1200], lockp[1250];
    snprintf(root_mj, sizeof root_mj, "%s/mod.json", proj_dir);
    snprintf(lockp, sizeof lockp, "%s/mod.lock", proj_dir);
    if (!fs_exists(root_mj)) {
        set_err(err, errcap, "no mod.json in %s (not a package project?)", proj_dir);
        return 1;
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
    return 0;
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
