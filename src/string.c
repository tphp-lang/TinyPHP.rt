/*
 * TinyPHP.rt - a small PHP runtime
 * string.c - immutable strings + PHP 8 number<->string conversions
 */
#include "value.h"
#include "tphp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t str_hash_calc(const char *s, size_t len) {
    /* FNV-1a */
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h ? h : 1;
}

uint32_t tstr_hash(TStr *s) {
    if (!s->hash) s->hash = str_hash_calc(s->c, s->len);
    return s->hash;
}

TStr *tstr_alloc_raw(uint32_t len, bool interned); /* mem.c */

TStr *tstr_new(const char *s, size_t len) {
    if (len > UINT32_MAX - 1) { fprintf(stderr, "tphp: string too long\n"); exit((int)TPHP_E_NOMEM); }
    TStr *str = tstr_alloc_raw((uint32_t)len, false);
    memcpy(str->c, s, len);
    return str;
}

/* intern table: program-lifetime strings (keywords, names, literals).
 * M5: per-OS-thread tables — compile-time literals are shared via prog
 * pointers (INTERNED strings never refcount), runtime interning in worker
 * threads only affects dedup within that thread. */
typedef struct InternSlot { struct InternSlot *next; TStr *s; } InternSlot;
static _Thread_local InternSlot **intern_tab = NULL;
static _Thread_local uint32_t intern_bcap = 0;   /* 桶数（2 的幂） */
static _Thread_local uint32_t intern_n = 0;      /* 条目数 */

/* 均摊 O(1)：负载（条目/桶）达 2 时倍增 rehash。原固定 4096 桶在唯一键
 * 累积场景退化为 O(n²)——kv-fill 50 万键实测每插入 1038→3384ns 递增
 * （PHP 恒 ~90ns），根因即每桶链长随 n/4096 线性增长、每键 intern 都扫链。 */
static void intern_grow(void) {
    uint32_t nb = intern_bcap ? intern_bcap * 2 : 4096;
    InternSlot **nt = tm_malloc(sizeof(InternSlot *) * nb);
    for (uint32_t i = 0; i < nb; i++) nt[i] = NULL;
    uint32_t mask = nb - 1;
    for (uint32_t i = 0; i < intern_bcap; i++) {
        InternSlot *p = intern_tab[i];
        while (p) {
            InternSlot *nx = p->next;
            uint32_t idx = p->s->hash & mask;
            p->next = nt[idx];
            nt[idx] = p;
            p = nx;
        }
    }
    if (intern_tab) tm_free(intern_tab);
    intern_tab = nt;
    intern_bcap = nb;
}

/* canonical decimal integer string? (PHP array-key rule: "8"→8, "08" stays
 * string, "-3"→-3, " 8" stays string). Used to set GC_FLAG_NONNUM_KEY. */
static bool str_is_canonical_int(const char *s, size_t len) {
    size_t i = 0;
    bool neg = false;
    if (len && s[0] == '-') { neg = true; i = 1; }
    if (i >= len) return false;
    for (size_t k = i; k < len; k++)
        if (s[k] < '0' || s[k] > '9') return false;
    if (len - i > 1 && s[i] == '0') return false;  /* leading zero */
    return true;
}

TStr *tstr_intern(const char *s, size_t len) {
    uint32_t h = str_hash_calc(s, len);
    if (intern_n * 2 >= intern_bcap) intern_grow();
    uint32_t idx = h & (intern_bcap - 1);
    for (InternSlot *p = intern_tab[idx]; p; p = p->next) {
        if (p->s->len == len && memcmp(p->s->c, s, len) == 0) return p->s;
    }
    TStr *str = tstr_alloc_raw((uint32_t)len, true);
    memcpy(str->c, s, len);
    str->hash = h;
    /* cache "not a canonical int key" so tval_norm_key skips digit scan */
    if (!str_is_canonical_int(s, len))
        str->h.flags |= GC_FLAG_NONNUM_KEY;
    InternSlot *slot = tm_malloc(sizeof(InternSlot));
    slot->s = str;
    slot->next = intern_tab[idx];
    intern_tab[idx] = slot;
    intern_n++;
    return str;
}

bool tstr_eq(TStr *a, TStr *b) {
    if (a == b) return true;
    if (a->len != b->len) return false;
    if (a->hash && b->hash && a->hash != b->hash) return false;
    return memcmp(a->c, b->c, a->len) == 0;
}

TStr *tstr_concat(TStr *a, TStr *b) {
    uint64_t len = (uint64_t)a->len + b->len;
    if (len > UINT32_MAX - 1) { fprintf(stderr, "tphp: string too long\n"); exit((int)TPHP_E_NOMEM); }
    TStr *s = tstr_alloc_raw((uint32_t)len, false);
    memcpy(s->c, a->c, a->len);
    memcpy(s->c + a->len, b->c, b->len);
    return s;
}

/* `.=` in-place（perf-roadmap）：dst 唯一引用且有预留容量时原地追加。
 * 仅限 T_STR 源（$s .= $x 主循环场景）；成功后 dst 原地生效、无需写回。 */
bool tstr_append_inplace(TStr *dst, const char *s, size_t n) {
    if (dst->h.flags & (GC_FLAG_INTERNED | GC_FLAG_MFCC)) return false;
    if (dst->h.rc != 1) return false;
    if ((uint64_t)dst->len + n > (uint64_t)dst->cap) return false;
    memcpy(dst->c + dst->len, s, n);
    dst->len += (uint32_t)n;
    dst->c[dst->len] = 0;
    dst->hash = 0;   /* 内容变了，失效缓存哈希 */
    return true;
}

/* ---- number -> string, PHP 8 semantics (echo/print/var_dump) ---- */
TStr *tphp_int_to_str(int64_t i) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)i);
    return tstr_new(buf, (size_t)n);
}

/* PHP uses precision=14 (zend_gcvt): like %.14G but guarantees a digit
 * after the decimal point in exponent form ("1.0E+15", not "1E+15") and
 * strips leading zeros from the exponent ("1.0E-5", not "1.0E-05"). */
TStr *tphp_float_to_str(double d) {
    char buf[64];
    if (isnan(d)) return tstr_intern("NAN", 3);
    if (isinf(d)) return d > 0 ? tstr_intern("INF", 3) : tstr_intern("-INF", 4);
    int n = snprintf(buf, sizeof buf, "%.14G", d);
    /* fix up exponent form: strip leading zeros of the exponent and
     * ensure ".0" before E when the mantissa has no dot */
    char *e = memchr(buf, 'E', (size_t)n);
    if (e) {
        char tmp[80];
        size_t mant = (size_t)(e - buf);
        char sign = 0;
        const char *ep = e + 1;
        if (*ep == '+' || *ep == '-') { sign = *ep; ep++; }
        while (ep[0] == '0' && ep[1] != 0) ep++;
        bool has_dot = memchr(buf, '.', mant) != NULL;
        int w;
        if (sign)
            w = has_dot ? snprintf(tmp, sizeof tmp, "%.*sE%c%s", (int)mant, buf, sign, ep)
                        : snprintf(tmp, sizeof tmp, "%.*s.0E%c%s", (int)mant, buf, sign, ep);
        else
            w = has_dot ? snprintf(tmp, sizeof tmp, "%.*sE%s", (int)mant, buf, ep)
                        : snprintf(tmp, sizeof tmp, "%.*s.0E%s", (int)mant, buf, ep);
        return tstr_new(tmp, (size_t)w);
    }
    return tstr_new(buf, (size_t)n);
}

/* ---- string -> number ---- */
static bool num_scan(const char *s, size_t len, bool trailing_ws_ok,
                     bool *is_int, int64_t *iv, double *dv, size_t *consumed) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                       s[i] == '\r' || s[i] == '\v' || s[i] == '\f')) i++;
    size_t start = i;
    bool neg = false;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    size_t digits_start = i;
    uint64_t mant = 0; int ndigits = 0; bool overflow = false;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        unsigned d = (unsigned)(s[i] - '0');
        if (!overflow) {
            if (mant > (UINT64_MAX - d) / 10) overflow = true;
            else { mant = mant * 10 + d; if (mant > (uint64_t)INT64_MAX) overflow = true; }
        }
        if (ndigits < 19) ndigits++;
        i++;
    }
    bool is_float = false;
    if (i < len && s[i] == '.') {
        is_float = true; i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') i++;
    }
    if (i == digits_start || (i == digits_start + 1 && s[digits_start] == '.'))
        return false; /* no digits at all ("." or empty) */
    if (!is_float && i < len && (s[i] == 'e' || s[i] == 'E')) {
        size_t save = i; i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) i++;
        if (i < len && s[i] >= '0' && s[i] <= '9') { is_float = true; while (i < len && s[i] >= '0' && s[i] <= '9') i++; }
        else i = save;
    }
    if (is_float && ndigits == 0 && !(i > digits_start && s[digits_start] == '.' && i > digits_start + 1))
        return false;
    size_t num_end = i;
    if (trailing_ws_ok) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                           s[i] == '\r' || s[i] == '\v' || s[i] == '\f')) i++;
        if (i != len) return false;
    } else if (i != len) {
        return false;
    }
    *consumed = num_end - start;
    if (is_float || overflow) {
        *is_int = false;
        *dv = strtod(s + start, NULL);
        /* strtod sees the whole buffer; re-run on the numeric prefix only */
        char tmp[512];
        size_t cl = num_end - start < sizeof tmp - 1 ? num_end - start : sizeof tmp - 1;
        memcpy(tmp, s + start, cl); tmp[cl] = 0;
        *dv = strtod(tmp, NULL);
    } else {
        *is_int = true;
        *iv = neg ? -(int64_t)mant : (int64_t)mant;
    }
    return true;
}

bool tstr_is_numeric(const char *s, size_t len, bool *is_int, int64_t *iv, double *dv) {
    size_t consumed;
    /* PHP 8: leading whitespace ok, trailing whitespace NOT allowed */
    return num_scan(s, len, false, is_int, iv, dv, &consumed);
}

bool tstr_prefix_numeric(const char *s, size_t len, bool *is_int, int64_t *iv, double *dv) {
    size_t consumed;
    if (num_scan(s, len, false, is_int, iv, dv, &consumed)) return true;
    /* fall back: leading numeric prefix ("123abc" -> 123) */
    size_t p = 0;
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' ||
                       s[p] == '\r' || s[p] == '\v' || s[p] == '\f')) p++;
    size_t d0 = p;
    double d = strtod(s + p, NULL);
    /* strtod accepted something numeric at the front? */
    const char *q = s + p;
    if (*q == '+' || *q == '-') q++;
    bool any_digit = false;
    while (q < s + len && *q >= '0' && *q <= '9') { any_digit = true; q++; }
    if (q < s + len && *q == '.') { q++; while (q < s + len && *q >= '0' && *q <= '9') { any_digit = true; q++; } }
    if (!any_digit) return false;
    (void)d0;
    *is_int = false;
    *dv = d;
    return true;
}

int64_t tstr_prefix_int(const char *s, size_t len) {
    size_t p = 0;
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' ||
                       s[p] == '\r' || s[p] == '\v' || s[p] == '\f')) p++;
    /* collect leading [+-]?digits */
    size_t q = p;
    if (q < len && (s[q] == '+' || s[q] == '-')) q++;
    size_t d0 = q;
    while (q < len && s[q] >= '0' && s[q] <= '9') q++;
    if (q == d0) return 0;
    bool neg = s[p] == '-';
    uint64_t mant = 0; bool overflow = false;
    for (size_t k = d0; k < q; k++) {
        unsigned d = (unsigned)(s[k] - '0');
        if (mant > (UINT64_MAX - d) / 10) { overflow = true; break; }
        mant = mant * 10 + d;
        if (!neg && mant > (uint64_t)INT64_MAX) overflow = true;
        if (neg && mant > (uint64_t)INT64_MAX + 1) overflow = true;
    }
    if (overflow) {
        /* PHP casts leading-numeric overflow to... it truncates digits: do a
         * float round-trip like PHP's DVAL_TO_LVAL path */
        char tmp[512];
        size_t cl = q - p < sizeof tmp - 1 ? q - p : sizeof tmp - 1;
        memcpy(tmp, s + p, cl); tmp[cl] = 0;
        double d = strtod(tmp, NULL);
        if (d >= 9223372036854775808.0) return INT64_MAX;
        if (d <= -9223372036854775808.0) return INT64_MIN;
        return (int64_t)d;
    }
    return neg ? -(int64_t)mant : (int64_t)mant;
}

double tstr_prefix_float(const char *s, size_t len) {
    char tmp[512];
    size_t p = 0;
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' ||
                       s[p] == '\r' || s[p] == '\v' || s[p] == '\f')) p++;
    size_t cl = len - p < sizeof tmp - 1 ? len - p : sizeof tmp - 1;
    memcpy(tmp, s + p, cl); tmp[cl] = 0;
    return strtod(tmp, NULL);
}
