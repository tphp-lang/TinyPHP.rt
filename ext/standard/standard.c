/*
 * TinyPHP.rt - ext/standard/standard.c - PHP ext/standard 函数子集
 *
 * 迁自 ext/builtin/builtin.c（函数体保持原实现，仅去 static）+ 新增
 * strripos/basename/dirname。语义对拍 php-8.5.1（见 standard.h）。
 * 文案：新函数按 PHP 官方格式（含 "N given" / ValueError 全文）；
 * 迁移函数保持原有文案（不破坏既有 EXPECT）。
 */
#include "standard.h"
#include "builtin.h"     /* need_str / need_int / str_find */
#include "tphp.h"
#include <string.h>

#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)

/* ---- 迁自 builtin.c（实现不变） ---- */

TVal bi_strpos(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError", "strpos() expects at least 2 arguments");
    TStr *hay = need_str(vm, &args[0], "strpos", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "strpos", "#2 ($needle)");
    int64_t off = n >= 3 ? need_int(vm, &args[2], "strpos", "#3 ($offset)") : 0;
    if (off > (int64_t)hay->len)
        RAISE(vm, "ValueError",
              "strpos(): Argument #3 ($offset) must be contained in "
              "argument #1 ($haystack)");
    if (off < 0) return TV_FALSE;      /* PHP 8.5 实测：负 offset → false */
    int64_t r = str_find(hay, needle, off, false, false);
    return r < 0 ? TV_FALSE : TV_INT(r);
}

TVal bi_stripos(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError", "stripos() expects at least 2 arguments");
    TStr *hay = need_str(vm, &args[0], "stripos", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "stripos", "#2 ($needle)");
    int64_t off = n >= 3 ? need_int(vm, &args[2], "stripos", "#3 ($offset)") : 0;
    if (off > (int64_t)hay->len)
        RAISE(vm, "ValueError",
              "stripos(): Argument #3 ($offset) must be contained in "
              "argument #1 ($haystack)");
    if (off < 0) return TV_FALSE;
    int64_t r = str_find(hay, needle, off, true, false);
    return r < 0 ? TV_FALSE : TV_INT(r);
}

/* strrpos/strripos 共用：带 offset 的反向查找（PHP 8.5 对拍语义）。
 *  off 越界（|off| > len）→ ValueError；空 needle → len（负 off → len+off）；
 *  正 off → 起点 ≥ off；负 off → 起点 ≤ len+off（向小索引搜）。 */
static int64_t str_rfind_off(VM *vm, TStr *hay, TStr *needle, int64_t off,
                             bool ci, const char *fn) {
    int64_t hl = (int64_t)hay->len, nl = (int64_t)needle->len;
    if (off > hl || off < -hl)
        RAISE(vm, "ValueError",
              "%s(): Argument #3 ($offset) must be contained in "
              "argument #1 ($haystack)", fn);
    if (nl == 0) return off < 0 ? hl + off : hl;
    int64_t lo = off > 0 ? off : 0;
    int64_t hi = off < 0 ? hl + off : hl - nl;   /* 最大起点 */
    if (hi > hl - nl) hi = hl - nl;
    for (int64_t i = hi; i >= lo; i--) {
        bool ok = true;
        for (int64_t j = 0; j < nl; j++) {
            char a = hay->c[i + j], b = needle->c[j];
            if (ci) {
                a = (char)((a >= 'A' && a <= 'Z') ? a + 32 : a);
                b = (char)((b >= 'A' && b <= 'Z') ? b + 32 : b);
            }
            if (a != b) { ok = false; break; }
        }
        if (ok) return i;
    }
    return -1;
}

TVal bi_strrpos(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError", "strrpos() expects at least 2 arguments");
    if (n > 3) RAISE(vm, "ArgumentCountError",
                     "strrpos() expects at most 3 arguments, %d given", n);
    TStr *hay = need_str(vm, &args[0], "strrpos", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "strrpos", "#2 ($needle)");
    int64_t off = n >= 3 ? need_int(vm, &args[2], "strrpos", "#3 ($offset)") : 0;
    int64_t r = str_rfind_off(vm, hay, needle, off, false, "strrpos");
    return r < 0 ? TV_FALSE : TV_INT(r);
}

TVal bi_strripos(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError",
                     "strripos() expects at least 2 arguments, %d given", n);
    if (n > 3) RAISE(vm, "ArgumentCountError",
                     "strripos() expects at most 3 arguments, %d given", n);
    TStr *hay = need_str(vm, &args[0], "strripos", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "strripos", "#2 ($needle)");
    int64_t off = n >= 3 ? need_int(vm, &args[2], "strripos", "#3 ($offset)") : 0;
    int64_t r = str_rfind_off(vm, hay, needle, off, true, "strripos");
    return r < 0 ? TV_FALSE : TV_INT(r);
}

TVal bi_strtolower(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TStr *s = need_str(vm, &args[0], "strtolower", "#1 ($string)");
    TStr *out = tstr_alloc_raw(s->len, false);
    for (uint32_t i = 0; i < s->len; i++) {
        char c = s->c[i];
        out->c[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    out->c[s->len] = 0;
    return TV_STR(out);
}

TVal bi_strtoupper(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TStr *s = need_str(vm, &args[0], "strtoupper", "#1 ($string)");
    TStr *out = tstr_alloc_raw(s->len, false);
    for (uint32_t i = 0; i < s->len; i++) {
        char c = s->c[i];
        out->c[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    out->c[s->len] = 0;
    return TV_STR(out);
}

TVal bi_strrev(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TStr *s = need_str(vm, &args[0], "strrev", "#1 ($string)");
    TStr *out = tstr_alloc_raw(s->len, false);
    for (uint32_t i = 0; i < s->len; i++) out->c[i] = s->c[s->len - 1 - i];
    out->c[s->len] = 0;
    return TV_STR(out);
}

/* nl2br：br 插在每个换行序列（\r\n | \n | 孤立 \r）之前（PHP 8.5 对拍：
 * hex 613c6272202f3e0d0a62 = "a<br />\r\nb"）。第二参 use_xhtml（默认
 * true："<br />"；false："<br>"）。 */
TVal bi_nl2br(VM *vm, TVal *args, int n) {
    if (n < 1) RAISE(vm, "ArgumentCountError",
                     "nl2br() expects at least 1 argument, %d given", n);
    if (n > 2) RAISE(vm, "ArgumentCountError",
                     "nl2br() expects at most 2 arguments, %d given", n);
    TStr *s = need_str(vm, &args[0], "nl2br", "#1 ($string)");
    bool xhtml = n >= 2 ? tval_to_bool(args[1]) : true;
    const char *br = xhtml ? "<br />" : "<br>";
    size_t brlen = xhtml ? 6 : 4;
    size_t nl = 0;
    for (size_t i = 0; i < s->len; i++)
        if (s->c[i] == '\n' || s->c[i] == '\r') nl++;
    char *b = tm_malloc(s->len + nl * (brlen + 1) + 1);
    size_t o = 0;
    for (size_t i = 0; i < s->len; i++) {
        char c = s->c[i];
        if (c == '\r' || c == '\n') {
            memcpy(b + o, br, brlen);
            o += brlen;
            b[o++] = c;
            if (c == '\r' && i + 1 < s->len && s->c[i + 1] == '\n') {
                b[o++] = '\n';
                i++;
            }
        } else {
            b[o++] = c;
        }
    }
    TStr *r = tstr_new(b, o);
    tm_free(b);
    return TV_STR(r);
}

/* ---- path（新增，PHP 8.5 对拍） ---- */

static bool path_sep(char c) {
#ifdef _WIN32
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

TVal bi_basename(VM *vm, TVal *args, int n) {
    if (n < 1) RAISE(vm, "ArgumentCountError",
                     "basename() expects at least 1 argument, %d given", n);
    if (n > 2) RAISE(vm, "ArgumentCountError",
                     "basename() expects at most 2 arguments, %d given", n);
    TStr *p = need_str(vm, &args[0], "basename", "#1 ($path)");
    TStr *suf = NULL;
    if (n >= 2) suf = need_str(vm, &args[1], "basename", "#2 ($suffix)");

    /* 剥尾部分隔符（全剥空 → ""，如 "/"、"///"） */
    size_t end = p->len;
    while (end > 0 && path_sep(p->c[end - 1])) end--;
    if (end == 0) return TV_STR(tstr_new("", 0));
    /* 最后一个分隔符之后 = 文件名段 */
    size_t start = 0;
    for (size_t i = 0; i < end; i++)
        if (path_sep(p->c[i])) start = i + 1;
    size_t seglen = end - start;
    /* suffix：仅当 suffix 长度严格小于段长且段尾匹配才剥（PHP 用 <
     * 比较——整段等于 suffix 时不剥：basename("abc","abc")="abc"） */
    if (suf && suf->len > 0 && suf->len < seglen &&
        memcmp(p->c + end - suf->len, suf->c, suf->len) == 0)
        seglen -= suf->len;
    return TV_STR(tstr_new(p->c + start, seglen));
}

/* dirname 单级上跳（Windows 规则对拍 php-8.5.1：
 *  根表示 = "\"（含盘符则 "X:\"）；"C:x" 无分隔符盘符 → "C:."；
 *  中间段保留原分隔符风格（"C:/a/b"→"C:/a"）。POSIX：无分隔符 → "."，
 *  根 → "/"。 */
static void dirname_up_once(char *buf, size_t *len) {
    size_t end = *len;
    bool stripped = false;
    while (end > 0 && path_sep(buf[end - 1])) { end--; stripped = true; }
    if (end == 0) {
        /* 全剥空：根。Windows 保留盘符（"C:\" → 盘符+"\"） */
#ifdef _WIN32
        if (*len >= 2 && buf[1] == ':') {
            buf[0] = buf[0]; buf[1] = ':';
            buf[2] = '\\';
            *len = 3;
        } else {
            buf[0] = '\\';
            *len = 1;
        }
#else
        buf[0] = '/';
        *len = 1;
#endif
        return;
    }
    size_t cut = end;
    while (cut > 0 && !path_sep(buf[cut - 1])) cut--;
    if (cut == 0) {
        /* 无分隔符 */
#ifdef _WIN32
        if (end >= 2 && buf[1] == ':') {
            /* 盘符形态：曾剥过尾分隔符 → "X:\"（盘符根），否则 "X:." */
            if (stripped) { buf[2] = '\\'; *len = 3; }
            else { buf[2] = '.'; *len = 3; }
        } else {
            buf[0] = '.';
            *len = 1;
        }
#else
        buf[0] = '.';
        *len = 1;
#endif
        return;
    }
    /* cut-1 是分隔符：结果 [0, cut-1)（cut==1 → 根） */
    size_t rlen = cut - 1;
#ifdef _WIN32
    if (rlen == 0) {
        buf[0] = '\\';
        *len = 1;
        return;
    }
    if (rlen == 2 && buf[1] == ':') {
        buf[2] = '\\';      /* "C:" → "C:\"（盘符根） */
        *len = 3;
        return;
    }
#else
    if (rlen == 0) {
        buf[0] = '/';
        *len = 1;
        return;
    }
#endif
    *len = rlen;
}

TVal bi_dirname(VM *vm, TVal *args, int n) {
    if (n < 1) RAISE(vm, "ArgumentCountError",
                     "dirname() expects at least 1 argument, %d given", n);
    if (n > 2) RAISE(vm, "ArgumentCountError",
                     "dirname() expects at most 2 arguments, %d given", n);
    TStr *p = need_str(vm, &args[0], "dirname", "#1 ($path)");
    int64_t levels = 1;
    if (n >= 2) {
        levels = need_int(vm, &args[1], "dirname", "#2 ($levels)");
        if (levels < 1)
            RAISE(vm, "ValueError",
                  "dirname(): Argument #2 ($levels) must be greater than "
                  "or equal to 1");
    }
    char buf[1200];
    if (p->len >= sizeof buf)
        RAISE(vm, "ValueError", "dirname(): path too long");
    memcpy(buf, p->c, p->len);
    size_t len = p->len;
    /* 至少一级；之后仅当结果仍在缩短才继续（PHP _zend_dirname 循环） */
    dirname_up_once(buf, &len);
    while (levels > 1) {
        size_t prev = len;
        dirname_up_once(buf, &len);
        if (len >= prev) break;
        levels--;
    }
    return TV_STR(tstr_new(buf, len));
}
