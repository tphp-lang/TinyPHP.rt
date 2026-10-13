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

static bool sha1_file(const char *path, char hex[41]) {
    FILE *fp = fopen(path, "rb");
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

/* ---- 通用 IO ---- */

static bool load_file(const char *path, uint8_t **out, size_t *out_len) {
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

static bool copy_file(const char *src, const char *dst) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (!load_file(src, &buf, &len)) return false;
    FILE *f = fopen(dst, "wb");
    if (!f) { free(buf); return false; }
    size_t w = fwrite(buf, 1, len, f);
    fclose(f);
    free(buf);
    return w == len;
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
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s\\*", base_abs);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        char abs[1024], rel[1024];
        snprintf(abs, sizeof abs, "%s\\%s", base_abs, fd.cFileName);
        if (base_rel[0])
            snprintf(rel, sizeof rel, "%s/%s", base_rel, fd.cFileName);
        else
            snprintf(rel, sizeof rel, "%s", fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            collect_dir(l, abs, rel, depth + 1);
        } else {
            if (ends_with_ci(fd.cFileName, ".php")) {
                fprintf(stderr, "tphp: warning: skipping .php file '%s'\n", rel);
                continue;
            }
            if (!relpath_safe(rel)) {
                fprintf(stderr, "tphp: warning: unsafe path skipped '%s'\n", rel);
                continue;
            }
            pl_add(l, rel, abs);
        }
    } while (FindNextFileA(h, &fd));
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
        struct stat st;
        if (stat(abs, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collect_dir(l, abs, rel, depth + 1);
        } else {
            if (ends_with_ci(e->d_name, ".php")) {
                fprintf(stderr, "tphp: warning: skipping .php file '%s'\n", rel);
                continue;
            }
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

static void collect_with(PackList *l, const char *p) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(p);
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
        collect_dir(l, p, "", 0);
    } else {
        const char *base = p;
        for (const char *q = p; *q; q++)
            if (*q == '/' || *q == '\\') base = q + 1;
        if (ends_with_ci(base, ".php")) {
            fprintf(stderr, "tphp: warning: skipping .php file '%s'\n", base);
            return;
        }
        if (!relpath_safe(base)) {
            fprintf(stderr, "tphp: unsafe filename '%s'\n", base);
            exit(1);
        }
        pl_add(l, base, p);
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

static bool build_zip(PackList *list, const char *main_php, size_t main_len,
                      uint8_t **out_zip, size_t *out_len, char *tmp_path, size_t tmp_cap) {
    /* manifest.json */
    size_t mcap = 256 + (size_t)list->n * 256;
    char *manifest = malloc(mcap);
    size_t mlen = (size_t)snprintf(manifest, mcap, "[");
    for (int i = 0; i < list->n; i++) {
        PackEntry *e = &list->items[i];
        if (mlen + 512 >= mcap) {
            mcap *= 2;
            manifest = realloc(manifest, mcap);
        }
        mlen += (size_t)snprintf(manifest + mlen, mcap - mlen,
            "%s{\"relpath\":\"%s\",\"sha1\":\"%s\",\"size\":%zu}",
            i ? "," : "", e->rel, e->sha1, e->len);
    }
    snprintf(manifest + mlen, mcap - mlen, "]");

#ifdef _WIN32
    snprintf(tmp_path, tmp_cap, "build\\_pack_tmp.zip");
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

    if (!load_file(tmp_path, out_zip, out_len)) {
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
    HANDLE hrsc = BeginUpdateResourceA(exe_path, FALSE);
    if (!hrsc) {
        fprintf(stderr, "tphp: BeginUpdateResource failed for '%s' (err %lu)\n",
                exe_path, GetLastError());
        return false;
    }
    if (!UpdateResourceA(hrsc, RT_RCDATA, res_name,
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
    FILE *fp = fopen(path, "rb");
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
                  char **with_paths, int with_n) {
    if (!pe_is_x64(exe_path)) {
        fprintf(stderr, "tphp: '%s' is not a x64 PE\n", exe_path);
        return -1;
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
                  char **with_paths, int with_n) {
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

/* 读自身文件：exe_path 容量 cap */
static bool self_exe_path(char *buf, size_t cap) {
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)cap);
    return n > 0 && n < cap;
#elif defined(__linux__)
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
#ifdef _WIN32
    (void)exe_path; (void)out; (void)out_len;
    return false;   /* Windows 走 FindResource，不用文件读 */
#else
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
#endif /* _WIN32 */
}

/* ---- 运行期释放（跨平台共享） ---- */

static bool mkdir_p(const char *path) {
#ifdef _WIN32
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 3; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = c;
        }
    }
    return CreateDirectoryA(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
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

/* 从 zip 缓冲释放到 root；跳过 manifest.json 与 __main.php。
 * out_main: 若含 __main.php，malloc 并返回 */
static bool extract_zip(const uint8_t *zdata, size_t zlen,
                        const char *root, char **out_main, size_t *out_main_len) {
    if (out_main) { *out_main = NULL; *out_main_len = 0; }
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_mem(&zip, zdata, zlen, 0)) {
        fprintf(stderr, "tphp: bundle zip corrupt\n");
        return false;
    }
    mz_uint nf = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < nf; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (st.m_is_directory) continue;
        const char *rel = st.m_filename;
        if (strcmp(rel, "manifest.json") == 0) continue;

        size_t extracted = 0;
        void *data = mz_zip_reader_extract_to_heap(&zip, i, &extracted, 0);
        if (!data) continue;

        /* 嵌入主脚本 */
        if (strcmp(rel, "__main.php") == 0 && out_main && !*out_main) {
            char *buf = malloc(extracted + 1);
            if (buf) {
                memcpy(buf, data, extracted);
                buf[extracted] = 0;
                *out_main = buf;
                *out_main_len = extracted;
            }
            free(data);
            continue;
        }

        if (!relpath_safe(rel)) {
            fprintf(stderr, "tphp: zip-slip blocked: %s\n", rel);
            free(data);
            mz_zip_reader_end(&zip);
            return false;
        }

        char dest[2048];
        snprintf(dest, sizeof dest, "%s/%s", root, rel);
        for (char *p = dest; *p; p++) if (*p == '\\') *p = '/';

        /* 已存在且 sha1 一致则跳过 */
        char existing[41], want[41];
        sha1_buf(data, extracted, want);
        if (sha1_file(dest, existing) && strcmp(existing, want) == 0) {
            free(data);
            continue;
        }

        /* mkdir -p 父目录 */
        char dir[2048];
        snprintf(dir, sizeof dir, "%s", dest);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = 0;
            mkdir_p(dir);
        }

        /* 原子：写 .tmp → rename */
        char tmpf[2100];
        snprintf(tmpf, sizeof tmpf, "%s.tmp", dest);
        FILE *fp = fopen(tmpf, "wb");
        if (fp) {
            fwrite(data, 1, extracted, fp);
            fclose(fp);
#ifdef _WIN32
            MoveFileExA(tmpf, dest, MOVEFILE_REPLACE_EXISTING);
#else
            rename(tmpf, dest);
#endif
        } else {
            fprintf(stderr, "tphp: cannot write '%s'\n", tmpf);
        }
        free(data);
    }
    mz_zip_reader_end(&zip);
    return true;
}

/* ---- 运行期入口 ---- */

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

    /* TPHP_BUNDLE */
    HRSRC hr = FindResourceA(self, "TPHP_BUNDLE", RT_RCDATA);
    if (!hr) return true;
    HGLOBAL hg = LoadResource(self, hr);
    DWORD zsz = SizeofResource(self, hr);
    const void *zdata = LockResource(hg);
    if (!zdata || !zsz) return false;

    char exe_dir[MAX_PATH];
    GetModuleFileNameA(self, exe_dir, MAX_PATH);
    char *slash = strrchr(exe_dir, '\\');
    if (slash) *slash = 0;
    char root[MAX_PATH];
    snprintf(root, sizeof root, "%s\\runtime\\lib", exe_dir);

    /* Windows 上无 __main.php（用 TPHP_MAIN），extract 仍处理之无妨 */
    bool ok = extract_zip((const uint8_t *)zdata, zsz, root, NULL, NULL);
    SetDllDirectoryA(root);
    return ok;

#else /* Unix */
    char exe[1024];
    if (!self_exe_path(exe, sizeof exe)) return true;   /* 找不到自身：无嵌入 */

    uint8_t *zdata = NULL;
    size_t zlen = 0;
    if (!read_embedded_zip(exe, &zdata, &zlen))
        return true;   /* 无嵌入：零开销正常启动 */

    /* runtime/lib 相对 exe 目录 */
    char root[1200];
    char *slash = strrchr(exe, '/');
    if (slash) {
        *slash = 0;
        snprintf(root, sizeof root, "%s/runtime/lib", exe);
    } else {
        snprintf(root, sizeof root, "./runtime/lib");
    }

    char *mainbuf = NULL;
    size_t mainlen = 0;
    bool ok = extract_zip(zdata, zlen, root, &mainbuf, &mainlen);
    free(zdata);

    if (ok && out_main && mainbuf) {
        *out_main = mainbuf;
        *out_main_len = mainlen;
    } else {
        free(mainbuf);
    }
    return ok;
#endif
}
