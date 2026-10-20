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
 */
#include "pkg.h"
#include "value.h"
#include "tphp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

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
