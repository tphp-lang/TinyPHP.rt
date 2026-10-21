/*
 * TinyPHP.rt - pack.c - M6.1 原生资产打包（PLAN/03 §6.1，跨平台）
 *
 * 共享：SHA1、--with 收集、miniz zip 构建、逐条目释放（sha1 校验 + 原子 rename）
 * Windows: PE RCDATA TPHP_BUNDLE + TPHP_MAIN
 * Linux:   ELF section .tphp_bundle（objcopy 注入；失败回落 footer）
 * macOS:   文件尾 footer（[zip][u64 le size][magic "TPHPBND1"]）
 * Unix 主脚本：zip 内 __main.php；Windows 仍用 TPHP_MAIN 资源
 */
#include "pack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
/* pack.c 只用 mz_zip_* API；关掉 zlib 兼容包装层，避免 -Wunused-function。
 * 注意：不可定义 MINIZ_NO_INFLATE_APIS / MINIZ_NO_DEFLATE_APIS——
 * miniz 会连带 #define MINIZ_NO_ARCHIVE_APIS，把 zip API 一起裁掉。 */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#if defined(__linux__)
#include <elf.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

#define PACK_FOOTER_MAGIC "TPHPBND1"
#define PACK_FOOTER_SIZE 16   /* u64 zip_len + 8-byte magic */
#define BUNDLE_ELF_SEC ".tphp_bundle"

/* ---- SHA1（自包含） ---- */
typedef struct {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t buf[64];
    size_t n;
} SHA1;

static uint32_t rol(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_init(SHA1 *s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->bytes = 0; s->n = 0;
}

static void sha1_block(SHA1 *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_update(SHA1 *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->bytes += n;
    while (n) {
        size_t take = 64 - s->n;
        if (take > n) take = n;
        memcpy(s->buf + s->n, p, take);
        s->n += take; p += take; n -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(SHA1 *s, char hex[41]) {
    uint64_t bits = s->bytes * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha1_update(s, &z, 1);
    uint8_t bl[8];
    for (int i = 0; i < 8; i++) bl[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, bl, 8);
    for (int i = 0; i < 5; i++)
        sprintf(hex + i * 8, "%08x", s->h[i]);
}

static bool sha1_buf(const void *data, size_t n, char hex[41]) {
    SHA1 s;
    sha1_init(&s);
    sha1_update(&s, data, n);
    sha1_final(&s, hex);
    return true;
}

/* ---- 通用 IO（接口一律 UTF-8；Windows 落宽字符 API） ---- */

#ifdef _WIN32
static void u8_to_w(const char *u8, wchar_t *w, size_t cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, (int)cap)) w[0] = 0;
}
static void w_to_u8(const wchar_t *w, char *u8, size_t cap) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, (int)cap, NULL, NULL)) u8[0] = 0;
}
static FILE *fopen_u8(const char *path, const char *mode) {
    wchar_t wp[1024], wm[16];
    u8_to_w(path, wp, 1024);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16);
    return _wfopen(wp, wm);
}
static bool move_replace_u8(const char *from, const char *to) {
    wchar_t wf[1024], wt[1024];
    u8_to_w(from, wf, 1024);
    u8_to_w(to, wt, 1024);
    return MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) != 0;
}
#else
#define u8_to_w(u8, w, cap)      ((void)0)
#define w_to_u8(w, u8, cap)      ((void)0)
#define fopen_u8(path, mode)     fopen((path), (mode))
static bool move_replace_u8(const char *from, const char *to) {
    return rename(from, to) == 0;
}
#include <limits.h>
#endif

/* load_file: UTF-8 路径（源文件/--with 输入） */
static bool load_file(const char *path, uint8_t **out, size_t *out_len) {
    FILE *fp = fopen_u8(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return false; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return false; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = 0;
    *out = buf;
    *out_len = got;
    return true;
}

/* load_file_ansi: 仅用于 miniz 直接 fopen 写出的临时 zip（ACP 路径） */
static bool load_file_ansi(const char *path, uint8_t **out, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return false; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return false; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = 0;
    *out = buf;
    *out_len = got;
    return true;
}

/* 磁盘文件 sha1（UTF-8 路径）——DLL 已存在比对、跳过未变更用 */
static bool sha1_file(const char *path, char hex[41]) {
    FILE *fp = fopen_u8(path, "rb");
    if (!fp) return false;
    SHA1 s;
    sha1_init(&s);
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
        sha1_update(&s, buf, n);
    fclose(fp);
    sha1_final(&s, hex);
    return true;
}

/* zip-slip：relpath 归一化后必须仍在根内 */
static bool relpath_safe(const char *rel) {
    if (!rel || !rel[0]) return false;
    if (rel[0] == '/' || rel[0] == '\\') return false;
    if (strlen(rel) >= 2 && rel[1] == ':') return false;
    const char *p = rel;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == 0))
            return false;
        while (*p && *p != '/' && *p != '\\') p++;
        if (*p) p++;
    }
    return true;
}

static bool ends_with_ci(const char *s, const char *suf) {
    size_t n = strlen(s), m = strlen(suf);
    if (n < m) return false;
    for (size_t i = 0; i < m; i++) {
        char a = s[n - m + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

/* vendor 自动打包排除（PLAN/pkg-manager §5.3）：
 * 恒跳过 VCS/系统垃圾；strict（自动 --with vendor/）再跳 tests/、*.md。
 * 显式 --with 时 strict=false，仅保留 VCS/系统垃圾跳过。 */
static bool g_vendor_strict = false;
void tphp_pack_set_vendor_strict(bool on) { g_vendor_strict = on; }

static bool pack_skip_name(const char *name) {
    if (strcmp(name, ".git") == 0 || strcmp(name, ".gitignore") == 0 ||
        strcmp(name, ".gitattributes") == 0 || strcmp(name, ".DS_Store") == 0 ||
        strcmp(name, "Thumbs.db") == 0)
        return true;
    if (g_vendor_strict) {
        if (strcmp(name, "tests") == 0 || strcmp(name, "test") == 0) return true;
        if (ends_with_ci(name, ".md")) return true;
    }
    return false;
}

/* ---- 收集 --with 条目 ---- */

typedef struct {
    char *rel;
    char *abs;
    uint8_t *data;
    size_t len;
    char sha1[41];
} PackEntry;

typedef struct {
    PackEntry *items;
    int n, cap;
} PackList;

static void pl_add(PackList *l, const char *rel, const char *abs) {
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->items = realloc(l->items, sizeof(PackEntry) * (size_t)l->cap);
    }
    PackEntry *e = &l->items[l->n++];
    memset(e, 0, sizeof *e);
    e->rel = malloc(strlen(rel) + 1);
    strcpy(e->rel, rel);
    e->abs = malloc(strlen(abs) + 1);
    strcpy(e->abs, abs);
}

#ifdef _WIN32
static void collect_dir(PackList *l, const char *base_abs, const char *base_rel,
                        int depth) {
    if (depth > 32) return;
    wchar_t wbase[768], pattern[800];
    u8_to_w(base_abs, wbase, 768);
    if (!wbase[0]) return;
    _snwprintf(pattern, 800, L"%s\\*", wbase);
    pattern[799] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        char fname[520];
        w_to_u8(fd.cFileName, fname, sizeof fname);
        char abs[2048], rel[2048];
        snprintf(abs, sizeof abs, "%s\\%s", base_abs, fname);
        if (base_rel[0])
            snprintf(rel, sizeof rel, "%s/%s", base_rel, fname);
        else
            snprintf(rel, sizeof rel, "%s", fname);
        if (pack_skip_name(fname)) continue;   /* VCS/垃圾/vendor 排除 */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect_dir(l, abs, rel, depth + 1);
        } else {
            if (!relpath_safe(rel)) {
                fprintf(stderr, "tphp: warning: unsafe path skipped '%s'\n", rel);
                continue;
            }
            pl_add(l, rel, abs);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#else
static void collect_dir(PackList *l, const char *base_abs, const char *base_rel,
                        int depth) {
    if (depth > 32) return;
    DIR *d = opendir(base_abs);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char abs[1024], rel[1024];
        snprintf(abs, sizeof abs, "%s/%s", base_abs, e->d_name);
        if (base_rel[0])
            snprintf(rel, sizeof rel, "%s/%s", base_rel, e->d_name);
        else
            snprintf(rel, sizeof rel, "%s", e->d_name);
        if (pack_skip_name(e->d_name)) continue;   /* VCS/垃圾/vendor 排除 */
        struct stat st;
        if (stat(abs, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_dir(l, abs, rel, depth + 1);
        } else {
            if (!relpath_safe(rel)) {
                fprintf(stderr, "tphp: warning: unsafe path skipped '%s'\n", rel);
                continue;
            }
            pl_add(l, rel, abs);
        }
    }
    closedir(d);
}
#endif

/* 入参路径 → 包内基准：剥 "./"、"\"→"/"、去尾分隔符（保持传入路径原样） */
static void path_to_rel_base(const char *p, char *out, size_t cap) {
    if (p[0] == '.' && (p[1] == '/' || p[1] == '\\'))
        p += 2;
    size_t i = 0;
    for (; p[i] && i + 1 < cap; i++)
        out[i] = p[i] == '\\' ? '/' : p[i];
    out[i] = 0;
    size_t n = strlen(out);
    while (n && out[n - 1] == '/') out[--n] = 0;
}

static bool path_is_abs(const char *p) {
    return p[0] == '/' || p[0] == '\\' || (strlen(p) >= 2 && p[1] == ':');
}

/* Source root for --with key computation and the __source_root zip entry.
 * Set once per tphp_pack_exe() call (from the main script's absolute dir).
 * Keys become source-root-relative so __DIR__-relative io can re-root a
 * disk-miss into the bundle (T12/T13). */
static char g_src_root[1024] = "";

/* Build-time path of the main script, written to the __main_path zip entry.
 * The runtime folds __FILE__/__DIR__ from this instead of "<embedded>",
 * which would make __DIR__ == "." and break the source_root strip (T13). */
static char g_main_path[1024] = "";

/* Resolve `in` to an absolute, separator-normalized path (forward slashes).
 * Best-effort: falls back to the input if the platform call fails. */
static void pack_abspath(const char *in, char *out, size_t cap) {
    out[0] = 0;
    if (!in || !in[0]) return;
#ifdef _WIN32
    wchar_t wbuf[1024], wfull[1024];
    u8_to_w(in, wbuf, 1024);
    DWORD n = wbuf[0] ? GetFullPathNameW(wbuf, 1024, wfull, NULL) : 0;
    if (n == 0 || n >= 1024) { snprintf(out, cap, "%s", in); }
    else { w_to_u8(wfull, out, cap); }
#else
    char full[1024];
    if (!realpath(in, full)) { snprintf(out, cap, "%s", in); }
    else { snprintf(out, cap, "%s", full); }
#endif
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
}

static void set_src_root(const char *dir) {
    if (!dir || !dir[0]) { g_src_root[0] = 0; return; }
    size_t n = 0;
    for (; dir[n] && n + 1 < sizeof g_src_root; n++)
        g_src_root[n] = dir[n] == '\\' ? '/' : dir[n];
    g_src_root[n] = 0;
    while (n > 1 && g_src_root[n - 1] == '/') g_src_root[--n] = 0; /* no trail */
}

/* If `p` (relative to cwd, or absolute) resolves under g_src_root, write the
 * source-root-relative key (forward slashes) and return true. */
static bool key_from_src_root(const char *p, char *out, size_t cap) {
    if (!g_src_root[0]) return false;
    char abs[1024];
    pack_abspath(p, abs, sizeof abs);
    size_t rl = strlen(g_src_root);
    if (strncmp(abs, g_src_root, rl) != 0) return false;
    if (abs[rl] != '/' && abs[rl] != 0) return false;
    const char *rel = abs + rl;
    while (*rel == '/') rel++;
    if (!rel[0]) return false;
    snprintf(out, cap, "%s", rel);
    return true;
}

static void collect_with(PackList *l, const char *p) {
#ifdef _WIN32
    wchar_t wp[1024];
    u8_to_w(p, wp, 1024);
    DWORD attrs = wp[0] ? GetFileAttributesW(wp) : INVALID_FILE_ATTRIBUTES;
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "tphp: --with path not found: %s\n", p);
        exit(1);
    }
    bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (stat(p, &st) != 0) {
        fprintf(stderr, "tphp: --with path not found: %s\n", p);
        exit(1);
    }
    bool is_dir = S_ISDIR(st.st_mode);
#endif
    if (is_dir) {
        char relbase[1024];
        if (key_from_src_root(p, relbase, sizeof relbase)) {
            /* under source root: key is source-root-relative (T12) */
        } else if (path_is_abs(p)) {
            /* 绝对路径无包内等效前缀：按目录内部相对收集（旧锚点行为） */
            relbase[0] = 0;
        } else {
            path_to_rel_base(p, relbase, sizeof relbase);
            if (strcmp(relbase, ".") == 0) relbase[0] = 0;   /* --with . */
            if (relbase[0] && !relpath_safe(relbase)) {
                fprintf(stderr, "tphp: warning: --with dir '%s' not "
                                "relative-safe; skipped\n", p);
                return;
            }
        }
        collect_dir(l, p, relbase, 0);
    } else {
        char rel[1024];
        if (key_from_src_root(p, rel, sizeof rel)) {
            /* under source root: key is source-root-relative (T12) */
        } else if (path_is_abs(p)) {
            /* 绝对路径无法作包内相对名 → 退回文件名 */
            const char *base = p;
            for (const char *q = p; *q; q++)
                if (*q == '/' || *q == '\\') base = q + 1;
            snprintf(rel, sizeof rel, "%s", base);
        } else {
            path_to_rel_base(p, rel, sizeof rel);
        }
        if (pack_skip_name(rel)) return;   /* VCS/垃圾/vendor 排除 */
        if (!relpath_safe(rel)) {
            fprintf(stderr, "tphp: unsafe filename '%s'\n", rel);
            exit(1);
        }
        pl_add(l, rel, p);
    }
}

static void packlist_load(PackList *l) {
    for (int i = 0; i < l->n; i++) {
        PackEntry *e = &l->items[i];
        if (!load_file(e->abs, &e->data, &e->len)) {
            fprintf(stderr, "tphp: cannot read '%s'\n", e->abs);
            exit(1);
        }
        sha1_buf(e->data, e->len, e->sha1);
    }
}

static void packlist_free(PackList *l) {
    for (int i = 0; i < l->n; i++) {
        free(l->items[i].rel);
        free(l->items[i].abs);
        free(l->items[i].data);
    }
    free(l->items);
    l->items = NULL;
    l->n = l->cap = 0;
}

/* ---- zip 构建（写临时文件再读回，跨平台） ---- */

/* dlopen 类后缀：运行期按等效路径提取到磁盘；其余走内存直读 */
static bool is_dll_name(const char *rel) {
    return ends_with_ci(rel, ".dll") || ends_with_ci(rel, ".so") ||
           ends_with_ci(rel, ".dylib");
}

/* JSON 字符串转义（relpath 进 manifest）；返回写入长度（不含 \0）。
 * out 至少 2*strlen(s)+8 字节。 */
static size_t json_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char esc[8];
        const char *e = NULL;
        size_t el = 0;
        if (*p == '"') e = "\\\"", el = 2;
        else if (*p == '\\') e = "\\\\", el = 2;
        else if (*p == '\n') e = "\\n", el = 2;
        else if (*p == '\r') e = "\\r", el = 2;
        else if (*p == '\t') e = "\\t", el = 2;
        else if (*p < 0x20) {
            snprintf(esc, sizeof esc, "\\u%04x", *p);
            e = esc; el = 6;
        } else {
            esc[0] = (char)*p; e = esc; el = 1;
        }
        if (o + el + 1 > cap) break;
        memcpy(out + o, e, el);
        o += el;
    }
    out[o] = 0;
    return o;
}

static bool build_zip(PackList *list, const char *main_php, size_t main_len,
                      uint8_t **out_zip, size_t *out_len, char *tmp_path, size_t tmp_cap) {
    /* manifest.json：运行期据此定位 DLL（type=dll）并做 sha1 校验 */
    size_t mcap = 256 + (size_t)list->n * 256;
    char *manifest = malloc(mcap);
    char esc[2048];
    size_t mlen = (size_t)snprintf(manifest, mcap, "[");
    for (int i = 0; i < list->n; i++) {
        PackEntry *e = &list->items[i];
        json_escape(e->rel, esc, sizeof esc);
        while (mlen + strlen(esc) + 128 >= mcap) {
            mcap *= 2;
            manifest = realloc(manifest, mcap);
        }
        mlen += (size_t)snprintf(manifest + mlen, mcap - mlen,
            "%s{\"relpath\":\"%s\",\"sha1\":\"%s\",\"size\":%zu,\"type\":\"%s\"}",
            i ? "," : "", esc, e->sha1, e->len, is_dll_name(e->rel) ? "dll" : "data");
    }
    snprintf(manifest + mlen, mcap - mlen, "]");

#ifdef _WIN32
    /* 系统临时目录（对齐 Unix /tmp）：不能假设 CWD 下有 build\ */
    {
        char tdir[MAX_PATH];
        DWORD n = GetTempPathA((DWORD)sizeof tdir, tdir);
        if (n == 0 || n >= sizeof tdir) {
            fprintf(stderr, "tphp: cannot resolve temp directory\n");
            free(manifest);
            return false;
        }
        snprintf(tmp_path, tmp_cap, "%stphp_pack_%lu.zip", tdir,
                 (unsigned long)GetCurrentProcessId());
    }
#else
    snprintf(tmp_path, tmp_cap, "/tmp/tphp_pack_%d.zip", (int)getpid());
#endif

    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_writer_init_file(&zip, tmp_path, 0)) {
        fprintf(stderr, "tphp: zip file init failed\n");
        free(manifest);
        return false;
    }
    mz_zip_writer_add_mem(&zip, "manifest.json", manifest, strlen(manifest),
                          MZ_DEFAULT_COMPRESSION);
    /* source root (absolute, forward slashes): lets the io layer re-root a
     * disk-miss into the bundle for __DIR__-relative reads (T12/T13) */
    if (g_src_root[0])
        mz_zip_writer_add_mem(&zip, "__source_root", g_src_root, strlen(g_src_root),
                              MZ_DEFAULT_COMPRESSION);
    /* main script build-time path: runtime folds __FILE__/__DIR__ from this
     * instead of "<embedded>" (T13) */
    if (g_main_path[0])
        mz_zip_writer_add_mem(&zip, "__main_path", g_main_path, strlen(g_main_path),
                              MZ_DEFAULT_COMPRESSION);
    if (main_php && main_len)
        mz_zip_writer_add_mem(&zip, "__main.php", main_php, main_len,
                              MZ_DEFAULT_COMPRESSION);
    for (int i = 0; i < list->n; i++) {
        PackEntry *e = &list->items[i];
        mz_zip_writer_add_mem(&zip, e->rel, e->data, e->len,
                              MZ_DEFAULT_COMPRESSION);
    }
    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
    free(manifest);

    if (!load_file_ansi(tmp_path, out_zip, out_len)) {
        fprintf(stderr, "tphp: cannot read temp zip\n");
        return false;
    }
    remove(tmp_path);
    return true;
}

/* ---- 嵌入（按平台） ---- */

#ifdef _WIN32

static bool write_resource(const char *exe_path, const char *res_name,
                           const void *data, size_t len) {
    wchar_t wexe[1024], wres[128];
    u8_to_w(exe_path, wexe, 1024);
    MultiByteToWideChar(CP_UTF8, 0, res_name, -1, wres, 128);
    HANDLE hrsc = BeginUpdateResourceW(wexe, FALSE);
    if (!hrsc) {
        fprintf(stderr, "tphp: BeginUpdateResource failed for '%s' (err %lu)\n",
                exe_path, GetLastError());
        return false;
    }
    if (!UpdateResourceW(hrsc, MAKEINTRESOURCEW(10) /* RT_RCDATA */, wres,
                         MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                         (LPVOID)data, (DWORD)len)) {
        fprintf(stderr, "tphp: UpdateResource failed (err %lu)\n", GetLastError());
        EndUpdateResource(hrsc, TRUE);
        return false;
    }
    if (!EndUpdateResource(hrsc, FALSE)) {
        fprintf(stderr, "tphp: EndUpdateResource failed (err %lu)\n", GetLastError());
        return false;
    }
    return true;
}

static bool pe_is_x64(const char *path) {
    FILE *fp = fopen_u8(path, "rb");
    if (!fp) return false;
    uint8_t mz[2];
    if (fread(mz, 1, 2, fp) != 2 || mz[0] != 'M' || mz[1] != 'Z') {
        fclose(fp); return false;
    }
    uint32_t pe_off = 0;
    fseek(fp, 0x3C, SEEK_SET);
    fread(&pe_off, 4, 1, fp);
    fseek(fp, (long)pe_off, SEEK_SET);
    char sig[4];
    if (fread(sig, 1, 4, fp) != 4 || memcmp(sig, "PE\0\0", 4) != 0) {
        fclose(fp); return false;
    }
    uint16_t machine = 0;
    fread(&machine, 2, 1, fp);
    fclose(fp);
    return machine == 0x8664;
}

int tphp_pack_exe(const char *exe_path,
                  const char *main_php, size_t main_php_len,
                  char **with_paths, int with_n,
                  const char *src_root, const char *main_path) {
    if (!pe_is_x64(exe_path)) {
        fprintf(stderr, "tphp: '%s' is not a x64 PE\n", exe_path);
        return -1;
    }
    set_src_root(src_root);
    g_main_path[0] = 0;
    if (main_path && main_path[0]) {
        char ab[1024];
        pack_abspath(main_path, ab, sizeof ab);
        snprintf(g_main_path, sizeof g_main_path, "%s", ab);
    }
    PackList list = {0};
    for (int i = 0; i < with_n; i++) collect_with(&list, with_paths[i]);
    packlist_load(&list);

    /* Windows: zip 只放资产；主脚本走独立 TPHP_MAIN 资源（不进 zip） */
    char tmp[512];
    uint8_t *zip = NULL;
    size_t zlen = 0;
    if (!build_zip(&list, NULL, 0, &zip, &zlen, tmp, sizeof tmp)) {
        packlist_free(&list);
        return -1;
    }

    bool ok = write_resource(exe_path, "TPHP_BUNDLE", zip, zlen);
    if (ok && main_php && main_php_len)
        ok = write_resource(exe_path, "TPHP_MAIN", main_php, main_php_len);

    int packed_n = list.n;
    free(zip);
    packlist_free(&list);
    if (ok)
        printf("pack: %d asset(s) + %s -> %s\n",
               packed_n, main_php ? "main" : "no-main", exe_path);
    return ok ? 0 : -1;
}

#else /* ---- Unix ---- */

/* 文件尾 footer：[zip bytes][u64 le size][8-byte magic] */
static bool footer_append(const char *exe_path, const void *zip, size_t zlen) {
    FILE *f = fopen(exe_path, "ab");
    if (!f) {
        fprintf(stderr, "tphp: cannot open '%s' for append\n", exe_path);
        return false;
    }
    uint64_t sz = (uint64_t)zlen;
    uint8_t le[8];
    for (int i = 0; i < 8; i++) le[i] = (uint8_t)(sz >> (i * 8));
    bool ok = fwrite(zip, 1, zlen, f) == zlen &&
              fwrite(le, 1, 8, f) == 8 &&
              fwrite(PACK_FOOTER_MAGIC, 1, 8, f) == 8;
    fclose(f);
    return ok;
}

/* Linux: objcopy 注入 ELF section；失败返回 false（调用方回落 footer） */
static bool elf_inject_section(const char *exe_path, const char *zip_path) {
    /* objcopy --add-section=NAME=FILE --set-section-flags NAME=contents,readonly,data EXE */
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "objcopy --add-section=" BUNDLE_ELF_SEC "=\"%s\" "
             "--set-section-flags " BUNDLE_ELF_SEC "=contents,readonly,data "
             "\"%s\" \"%s\" 2>/dev/null",
             zip_path, exe_path, exe_path);
    int rc = system(cmd);
    if (rc != 0) {
        /* 无 objcopy 或失败：回落 footer（需先去掉可能的部分写入——objcopy 失败不动原文件） */
        return false;
    }
    return true;
}

int tphp_pack_exe(const char *exe_path,
                  const char *main_php, size_t main_php_len,
                  char **with_paths, int with_n,
                  const char *src_root, const char *main_path) {
    set_src_root(src_root);
    g_main_path[0] = 0;
    if (main_path && main_path[0]) {
        char ab[1024];
        pack_abspath(main_path, ab, sizeof ab);
        snprintf(g_main_path, sizeof g_main_path, "%s", ab);
    }
    PackList list = {0};
    for (int i = 0; i < with_n; i++) collect_with(&list, with_paths[i]);
    packlist_load(&list);

    char tmp[512];
    uint8_t *zip = NULL;
    size_t zlen = 0;
    if (!build_zip(&list, main_php, main_php_len, &zip, &zlen, tmp, sizeof tmp)) {
        packlist_free(&list);
        return -1;
    }

    bool ok = false;
#if defined(__linux__)
    /* 优先 ELF section；objcopy 需要 zip 在磁盘——重写临时文件 */
    {
        FILE *zf = fopen(tmp, "wb");
        if (zf) { fwrite(zip, 1, zlen, zf); fclose(zf); }
        if (elf_inject_section(exe_path, tmp)) {
            ok = true;
            remove(tmp);
        } else {
            remove(tmp);
        }
    }
#endif
    if (!ok) {
        /* macOS / Linux 回落：文件尾 footer */
        ok = footer_append(exe_path, zip, zlen);
    }

    int packed_n = list.n;
    free(zip);
    packlist_free(&list);
    if (ok)
        printf("pack: %d asset(s) + %s -> %s%s\n",
               packed_n, main_php ? "main" : "no-main", exe_path,
               ok ? "" : " (footer)");
    return ok ? 0 : -1;
}

#endif /* _WIN32 */

/* ---- 运行期：查找嵌入 zip ---- */

/* 读自身文件：exe_path 容量 cap —— Unix 路径专用（Windows 走 GetModuleFileNameW） */
#ifndef _WIN32
static bool self_exe_path(char *buf, size_t cap) {
#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n <= 0) return false;
    buf[n] = 0;
    return true;
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)cap;
    if (_NSGetExecutablePath(buf, &sz) != 0) return false;
    return true;
#else
    (void)buf; (void)cap;
    return false;
#endif
}

/* 从 exe 文件读取嵌入 zip（ELF section 或 footer）；成功返回 malloc 缓冲 */
static bool read_embedded_zip(const char *exe_path, uint8_t **out, size_t *out_len) {
    FILE *fp = fopen(exe_path, "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    if (fsz < PACK_FOOTER_SIZE) { fclose(fp); return false; }

    /* 1) footer：尾部 magic */
    fseek(fp, fsz - PACK_FOOTER_SIZE, SEEK_SET);
    uint8_t footer[PACK_FOOTER_SIZE];
    if (fread(footer, 1, PACK_FOOTER_SIZE, fp) == PACK_FOOTER_SIZE &&
        memcmp(footer + 8, PACK_FOOTER_MAGIC, 8) == 0) {
        uint64_t zlen = 0;
        for (int i = 0; i < 8; i++) zlen |= (uint64_t)footer[i] << (i * 8);
        if (zlen == 0 || zlen > (uint64_t)fsz) { fclose(fp); return false; }
        uint8_t *buf = malloc((size_t)zlen);
        if (!buf) { fclose(fp); return false; }
        fseek(fp, fsz - PACK_FOOTER_SIZE - (long)zlen, SEEK_SET);
        bool ok = fread(buf, 1, (size_t)zlen, fp) == zlen;
        fclose(fp);
        if (ok) { *out = buf; *out_len = (size_t)zlen; return true; }
        free(buf);
        return false;
    }

#if defined(__linux__)
    /* 2) ELF section .tphp_bundle */
    rewind(fp);
    Elf64_Ehdr eh;
    if (fread(&eh, 1, sizeof eh, fp) == sizeof eh &&
        memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 &&
        eh.e_ident[EI_CLASS] == ELFCLASS64) {
        /* 读 section header 表 */
        Elf64_Shdr *sh = malloc(eh.e_shentsize * eh.e_shnum);
        if (sh) {
            fseek(fp, (long)eh.e_shoff, SEEK_SET);
            if (fread(sh, eh.e_shentsize, eh.e_shnum, fp) == eh.e_shnum) {
                /* 读 shstrtab */
                if (eh.e_shstrndx < eh.e_shnum) {
                    Elf64_Shdr *strtab = &sh[eh.e_shstrndx];
                    char *names = malloc(strtab->sh_size);
                    if (names) {
                        fseek(fp, (long)strtab->sh_offset, SEEK_SET);
                        if (fread(names, 1, strtab->sh_size, fp) == strtab->sh_size) {
                            for (int i = 0; i < eh.e_shnum; i++) {
                                const char *nm = names + sh[i].sh_name;
                                if (strcmp(nm, BUNDLE_ELF_SEC) == 0) {
                                    uint8_t *buf = malloc(sh[i].sh_size);
                                    if (buf) {
                                        fseek(fp, (long)sh[i].sh_offset, SEEK_SET);
                                        if (fread(buf, 1, sh[i].sh_size, fp) == sh[i].sh_size) {
                                            free(names); free(sh); fclose(fp);
                                            *out = buf;
                                            *out_len = (size_t)sh[i].sh_size;
                                            return true;
                                        }
                                        free(buf);
                                    }
                                    break;
                                }
                            }
                        }
                        free(names);
                    }
                }
            }
            free(sh);
        }
    }
#endif
    fclose(fp);
    return false;
}
#endif /* !_WIN32 */

/* ---- 运行期释放（跨平台共享） ---- */

static bool mkdir_p(const char *path) {
#ifdef _WIN32
    wchar_t tmp[1024];
    u8_to_w(path, tmp, 1024);
    if (!tmp[0]) return false;
    size_t n = wcslen(tmp);
    for (size_t i = (n > 3 ? 3 : 1); i < n; i++) {
        if (tmp[i] == L'/' || tmp[i] == L'\\') {
            wchar_t c = tmp[i];
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = c;
        }
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    if (n == 0) return false;
    /* 跳过首字符（绝对路径 /） */
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { tmp[i] = '/'; return false; }
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST;
#endif
}

/* ---- 运行期 bundle：zip 常驻内存（Bun 式），数据资产直读不落盘；
 *      仅 manifest 中 type=dll 的条目按等效相对路径提取到指纹缓存 ---- */

static uint8_t *g_bundle;         /* zip 常驻（Win=资源映射不拥有 / Unix=malloc） */
static size_t g_bundle_len;
static mz_zip_archive g_rd;       /* 读侧；startup 内 eager 初始化，读取加锁 */
static bool g_rd_ready;
static char g_dll_root[1024];     /* DLL 提取根（UTF-8 绝对路径）；空 = 无/未提取 */

static atomic_flag g_bundle_lock = ATOMIC_FLAG_INIT;

static void bundle_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_bundle_lock,
                                             memory_order_acquire)) {}
}
static void bundle_unlock(void) {
    atomic_flag_clear_explicit(&g_bundle_lock, memory_order_release);
}

/* zip 条目统一用 '/'（包内存储如此；PHP 侧可能给 '\'） */
static void norm_rel(const char *rel, char *out, size_t cap) {
    size_t i = 0;
    for (; rel[i] && i + 1 < cap; i++)
        out[i] = rel[i] == '\\' ? '/' : rel[i];
    out[i] = 0;
}

bool tphp_pack_has_bundle(void) {
    return g_bundle != NULL;
}

const char *tphp_pack_dll_root(void) {
    return g_dll_root[0] ? g_dll_root : NULL;
}

/* Source root recorded at build time (T12). Lazily read from the __source_root
 * zip entry and cached; returns "" when unpacked or when the entry is absent
 * (bundles built before T12). */
const char *tphp_pack_source_root(void) {
    static char root[1024] = "";
    static bool loaded = false;
    if (loaded) return root;
    loaded = true;
    if (!g_bundle) return root;
    void *data = NULL;
    size_t len = 0;
    if (!tphp_pack_read("__source_root", &data, &len)) return root;
    size_t n = len < sizeof root - 1 ? len : sizeof root - 1;
    memcpy(root, data, n);
    root[n] = 0;
    free(data);
    while (n > 0 && (root[n - 1] == '\n' || root[n - 1] == '\r')) root[--n] = 0;
    return root;
}

/* Build-time path of the embedded main script (T13). Lets the runtime fold
 * __FILE__/__DIR__ to the ORIGINAL source path instead of "<embedded>",
 * which would make __DIR__ resolve to dirname("<embedded>") == "." and break
 * the io layer's source_root strip. Returns "" when absent. */
const char *tphp_pack_main_path(void) {
    static char path[1024] = "";
    static bool loaded = false;
    if (loaded) return path;
    loaded = true;
    if (!g_bundle) return path;
    void *data = NULL;
    size_t len = 0;
    if (!tphp_pack_read("__main_path", &data, &len)) return path;
    size_t n = len < sizeof path - 1 ? len : sizeof path - 1;
    memcpy(path, data, n);
    path[n] = 0;
    free(data);
    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r')) path[--n] = 0;
    return path;
}

bool tphp_pack_read(const char *rel, void **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    if (!g_bundle || !rel) return false;
    char n[1024];
    norm_rel(rel, n, sizeof n);
    bundle_lock();
    if (!g_rd_ready) { bundle_unlock(); return false; }
    int idx = mz_zip_reader_locate_file(&g_rd, n, NULL, 0);
    if (idx < 0) { bundle_unlock(); return false; }
    size_t sz = 0;
    void *data = mz_zip_reader_extract_to_heap(&g_rd, (mz_uint)idx, &sz, 0);
    bundle_unlock();
    if (!data) return false;
    char *buf = malloc(sz + 1);          /* NUL 结尾：fopen 流/嵌入脚本直用 */
    if (!buf) { free(data); return false; }
    memcpy(buf, data, sz);
    buf[sz] = 0;
    free(data);
    *out = buf;
    *out_len = sz;
    return true;
}

bool tphp_pack_stat(const char *rel, size_t *out_size) {
    if (!g_bundle || !rel) return false;
    char n[1024];
    norm_rel(rel, n, sizeof n);
    bundle_lock();
    if (!g_rd_ready) { bundle_unlock(); return false; }
    int idx = mz_zip_reader_locate_file(&g_rd, n, NULL, 0);
    if (idx < 0) { bundle_unlock(); return false; }
    mz_zip_archive_file_stat st;
    bool ok = mz_zip_reader_file_stat(&g_rd, (mz_uint)idx, &st) != 0;
    bundle_unlock();
    if (ok && out_size) *out_size = st.m_uncomp_size;
    return ok;
}

static int cmp_strp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int tphp_pack_list(const char *dir_rel, char ***names) {
    *names = NULL;
    if (!g_bundle || !dir_rel) return 0;
    char prefix[1024];
    norm_rel(dir_rel, prefix, sizeof prefix);
    size_t pl = strlen(prefix);
    if (pl && prefix[pl - 1] == '/') pl--;
    bundle_lock();
    if (!g_rd_ready) { bundle_unlock(); return 0; }
    mz_uint nf = mz_zip_reader_get_num_files(&g_rd);
    char **out = NULL;
    int cnt = 0;
    for (mz_uint i = 0; i < nf; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&g_rd, i, &st)) continue;
        if (st.m_is_directory) continue;
        const char *nm = st.m_filename;
        if (pl) {
            if (strncmp(nm, prefix, pl) != 0 || nm[pl] != '/') continue;
            nm += pl + 1;
        }
        const char *slash = strchr(nm, '/');
        size_t seglen = slash ? (size_t)(slash - nm) : strlen(nm);
        if (seglen == 0) continue;
        bool dup = false;
        for (int j = 0; j < cnt; j++)
            if (strlen(out[j]) == seglen && memcmp(out[j], nm, seglen) == 0) {
                dup = true;
                break;
            }
        if (dup) continue;
        char *cp = malloc(seglen + 1);
        memcpy(cp, nm, seglen);
        cp[seglen] = 0;
        char **grown = realloc(out, sizeof(char *) * (size_t)(cnt + 1));
        if (!grown) { free(cp); break; }
        out = grown;
        out[cnt++] = cp;
    }
    bundle_unlock();
    if (out && cnt > 1) qsort(out, (size_t)cnt, sizeof(char *), cmp_strp);
    *names = out;
    return cnt;
}

/* ---- manifest.json 解析（只认构建端生成的固定 schema，失败即报） ---- */

static const char *m_ws(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static bool m_lit(const char **pp, const char *end, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(end - *pp) < n || memcmp(*pp, lit, n) != 0) return false;
    *pp += n;
    return true;
}

/* 解析 JSON 字符串（支持构建端 json_escape 的转义子集），pp 指向开引号 */
static bool m_string(const char **pp, const char *end, char *dst, size_t cap) {
    if (*pp >= end || **pp != '"') return false;
    (*pp)++;
    size_t o = 0;
    while (*pp < end && **pp != '"') {
        char c = **pp;
        if (c == '\\') {
            (*pp)++;
            if (*pp >= end) return false;
            char e = **pp;
            if (e == 'n') c = '\n';
            else if (e == 'r') c = '\r';
            else if (e == 't') c = '\t';
            else if (e == '"' || e == '\\') c = e;
            else if (e == 'u') {
                if (end - *pp < 5) return false;
                unsigned v = 0;
                for (int i = 1; i <= 4; i++) {
                    char h = (*pp)[i];
                    v <<= 4;
                    if (h >= '0' && h <= '9') v += (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v += (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v += (unsigned)(h - 'A' + 10);
                    else return false;
                }
                if (v > 0x7f) return false;   /* 构建端只生成 \u00XX 控制字符 */
                c = (char)v;
                *pp += 4;
            } else {
                return false;
            }
        } else if ((unsigned char)c < 0x20) {
            return false;
        }
        if (o + 1 >= cap) return false;
        dst[o++] = c;
        (*pp)++;
    }
    if (*pp >= end || **pp != '"') return false;
    (*pp)++;
    dst[o] = 0;
    return true;
}

static bool m_uint(const char **pp, const char *end, size_t *out) {
    if (*pp >= end || **pp < '0' || **pp > '9') return false;
    size_t v = 0;
    while (*pp < end && **pp >= '0' && **pp <= '9') {
        v = v * 10 + (size_t)(**pp - '0');
        (*pp)++;
    }
    *out = v;
    return true;
}

typedef struct { char rel[1024]; char sha1[41]; size_t size; } ManifestDll;

/* [{"relpath":"…","sha1":"…","size":N,"type":"dll|data"},…] → 仅收 dll */
static bool manifest_parse_dlls(const char *js, size_t len,
                                ManifestDll **out, int *nout) {
    const char *p = js, *end = js + len;
    ManifestDll *arr = NULL;
    int cnt = 0;
    *out = NULL;
    *nout = 0;
    p = m_ws(p, end);
    if (!m_lit(&p, end, "[")) return false;
    p = m_ws(p, end);
    if (p < end && *p == ']') return true;
    for (;;) {
        p = m_ws(p, end);
        if (!m_lit(&p, end, "{")) goto fail;
        char rel[1024], sha[41], type[16];
        if (!m_lit(&p, end, "\"relpath\":") || !m_string(&p, end, rel, sizeof rel))
            goto fail;
        if (!m_lit(&p, end, ",\"sha1\":") || !m_string(&p, end, sha, sizeof sha))
            goto fail;
        if (!m_lit(&p, end, ",\"size\":")) goto fail;
        size_t sz = 0;
        if (!m_uint(&p, end, &sz)) goto fail;
        if (!m_lit(&p, end, ",\"type\":") || !m_string(&p, end, type, sizeof type))
            goto fail;
        if (!m_lit(&p, end, "}")) goto fail;
        if (strcmp(type, "dll") == 0) {
            ManifestDll *grown = realloc(arr, sizeof(ManifestDll) * (size_t)(cnt + 1));
            if (!grown) goto fail;
            arr = grown;
            snprintf(arr[cnt].rel, sizeof arr[cnt].rel, "%s", rel);
            snprintf(arr[cnt].sha1, sizeof arr[cnt].sha1, "%s", sha);
            arr[cnt].size = sz;
            cnt++;
        }
        p = m_ws(p, end);
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; break; }
        goto fail;
    }
    *out = arr;
    *nout = cnt;
    return true;
fail:
    free(arr);
    return false;
}

/* exe 所在目录（UTF-8，无尾分隔符）——DLL 等效路径的落盘根 */
static bool exe_dir(char *out, size_t cap) {
#ifdef _WIN32
    wchar_t wp[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wp, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    char u[1024];
    w_to_u8(wp, u, sizeof u);
#else
    char u[1024];
    if (!self_exe_path(u, sizeof u)) return false;
#endif
    char *slash = strrchr(u, '/');
#ifdef _WIN32
    char *bslash = strrchr(u, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) *slash = 0;
    if (!u[0]) return false;
    snprintf(out, cap, "%s", u);
    return true;
}

/* 原子写：<dest>.tmp → rename/MoveFileEx（UTF-8 路径） */
static bool write_file_atomic(const char *dest, const void *data, size_t len) {
    char tmpf[2100];
    snprintf(tmpf, sizeof tmpf, "%s.tmp", dest);
    FILE *fp = fopen_u8(tmpf, "wb");
    if (!fp) {
        fprintf(stderr, "tphp: cannot write '%s'\n", tmpf);
        return false;
    }
    size_t w = fwrite(data, 1, len, fp);
    fclose(fp);
    if (w != len) {
        fprintf(stderr, "tphp: short write '%s'\n", tmpf);
        return false;
    }
    if (!move_replace_u8(tmpf, dest)) {
        fprintf(stderr, "tphp: cannot move '%s'\n", dest);
        return false;
    }
    return true;
}

/* 按 manifest 提取 DLL 到 exe 同级等效路径（<exedir>/<relpath>）。
 * 目标已存在且 sha1 与清单一致 → 跳过；否则校验 zip 侧 sha1 后原子写入。
 * 全部就绪才置 g_dll_root=exe 目录（FFI 据此拼等效绝对路径）。 */
static void bundle_extract_dlls(void) {
    void *mf = NULL;
    size_t mfl = 0;
    if (!tphp_pack_read("manifest.json", &mf, &mfl)) return;
    ManifestDll *dlls = NULL;
    int nd = 0;
    if (!manifest_parse_dlls(mf, mfl, &dlls, &nd)) {
        fprintf(stderr, "tphp: bundle manifest corrupt; DLLs not extracted\n");
        free(mf);
        return;
    }
    free(mf);
    if (nd == 0) { free(dlls); return; }

    char root[1024];
    if (!exe_dir(root, sizeof root)) {
        fprintf(stderr, "tphp: cannot resolve exe directory\n");
        free(dlls);
        return;
    }
    bool all_ok = true;
    for (int i = 0; i < nd && all_ok; i++) {
        if (!relpath_safe(dlls[i].rel)) {
            fprintf(stderr, "tphp: unsafe DLL path in manifest: %s\n", dlls[i].rel);
            all_ok = false;
            break;
        }
        /* 等效路径：<exedir>/<relpath>（包内结构在 exe 旁原样保留） */
        char dest[2048];
        snprintf(dest, sizeof dest, "%s/%s", root, dlls[i].rel);
        char disk_sha[41];
        if (sha1_file(dest, disk_sha) && strcmp(disk_sha, dlls[i].sha1) == 0)
            continue;                       /* 未变更 → 跳过 */
        void *data = NULL;
        size_t len = 0;
        if (!tphp_pack_read(dlls[i].rel, &data, &len)) {
            fprintf(stderr, "tphp: bundle DLL missing: %s\n", dlls[i].rel);
            all_ok = false;
            break;
        }
        char hex[41];
        sha1_buf(data, len, hex);
        if (strcmp(hex, dlls[i].sha1) != 0) {
            fprintf(stderr, "tphp: DLL integrity check failed: %s\n", dlls[i].rel);
            free(data);
            all_ok = false;
            break;
        }
        char dir[2048];
        snprintf(dir, sizeof dir, "%s", dest);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; mkdir_p(dir); }
        all_ok = write_file_atomic(dest, data, len);
        free(data);
    }
    if (all_ok)
        snprintf(g_dll_root, sizeof g_dll_root, "%s", root);
    free(dlls);
}

/* ---- 运行期入口 ---- */

/* zip 定位后的公共装配：读侧 eager 初始化 → 嵌入主脚本回退 → 提 DLL */
static bool bundle_mount(void) {
    g_rd_ready = mz_zip_reader_init_mem(&g_rd, g_bundle, g_bundle_len, 0) != 0;
    if (!g_rd_ready) {
        fprintf(stderr, "tphp: bundle zip corrupt\n");
        return false;
    }
    return true;
}

bool tphp_pack_startup(char **out_main, size_t *out_main_len) {
    if (out_main) { *out_main = NULL; *out_main_len = 0; }

#ifdef _WIN32
    HMODULE self = GetModuleHandleA(NULL);

    /* TPHP_MAIN：嵌入主脚本 */
    if (out_main) {
        HRSRC hr = FindResourceA(self, "TPHP_MAIN", RT_RCDATA);
        if (hr) {
            HGLOBAL hg = LoadResource(self, hr);
            DWORD sz = SizeofResource(self, hr);
            const char *p = LockResource(hg);
            if (p && sz) {
                char *buf = malloc(sz + 1);
                memcpy(buf, p, sz);
                buf[sz] = 0;
                *out_main = buf;
                *out_main_len = sz;
            }
        }
    }

    /* TPHP_BUNDLE：zip 常驻内存，不再整体落盘 */
    HRSRC hr = FindResourceA(self, "TPHP_BUNDLE", RT_RCDATA);
    if (!hr) return true;
    HGLOBAL hg = LoadResource(self, hr);
    DWORD zsz = SizeofResource(self, hr);
    const void *zdata = LockResource(hg);
    if (!zdata || !zsz) return false;

    g_bundle = (uint8_t *)(uintptr_t)zdata;
    g_bundle_len = zsz;
    if (!bundle_mount()) return false;

    if (out_main && !*out_main && !*out_main_len) {
        void *m = NULL;
        size_t ml = 0;
        if (tphp_pack_read("__main.php", &m, &ml)) {
            *out_main = m;
            *out_main_len = ml;
        }
    }
    bundle_extract_dlls();   /* DLL → exe 同级等效路径（依赖走应用目录默认搜索） */
    return true;

#else /* Unix */
    char exe[1024];
    if (!self_exe_path(exe, sizeof exe)) return true;   /* 找不到自身：无嵌入 */

    uint8_t *zdata = NULL;
    size_t zlen = 0;
    if (!read_embedded_zip(exe, &zdata, &zlen))
        return true;   /* 无嵌入：零开销正常启动 */

    g_bundle = zdata;
    g_bundle_len = zlen;           /* 进程级常驻（malloc 不回收） */
    if (!bundle_mount()) {
        free(zdata);
        g_bundle = NULL;
        return false;
    }

    if (out_main && !*out_main && !*out_main_len) {
        void *m = NULL;
        size_t ml = 0;
        if (tphp_pack_read("__main.php", &m, &ml)) {
            *out_main = m;
            *out_main_len = ml;
        }
    }
    bundle_extract_dlls();
    return true;
#endif
}
