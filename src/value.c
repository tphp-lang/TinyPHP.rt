/*
 * TinyPHP.rt - a small PHP runtime
 * value.c - conversions, loose comparison matrix (ported from php-src
 *           zend_operators.c semantics), arithmetic with numeric strings
 */
#include "value.h"
#include "tphp.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

extern void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
extern void tphp_raise(const char *ex_class_name, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

TVal TV_NULL = { .u.i = 0, .t = T_NULL };
TVal TV_FALSE = { .u.i = 0, .t = T_FALSE };
TVal TV_TRUE = { .u.i = 0, .t = T_TRUE };

ToStrHook _Thread_local tphp_tostr_hook = NULL;   /* installed by the VM (vm.c) */

/* ---- conversions ---- */
bool tval_to_bool(TVal v) {
    switch (v.t) {
    case T_NULL: case T_FALSE: return false;
    case T_TRUE: return true;
    case T_INT: return v.u.i != 0;
    case T_FLOAT: return v.u.d != 0.0;
    case T_STR: {
        TStr *s = v.u.p;
        if (s->len == 0) return false;
        if (s->len == 1 && s->c[0] == '0') return false;
        return true;
    }
    case T_ARR: return ((TArr *)v.u.p)->n != 0;
    default: return true;
    }
}

int64_t tval_to_int(TVal v) {
    switch (v.t) {
    case T_NULL: case T_FALSE: return 0;
    case T_TRUE: return 1;
    case T_INT: return v.u.i;
    case T_FLOAT: {
        double d = v.u.d;
        if (isnan(d))
            tphp_raise("ValueError", "NAN cannot be converted to int");
        if (d >= 9223372036854775808.0 || d <= -9223372036854775808.0)
            tphp_raise("ValueError", "Cannot convert float to int, value out of range");
        return (int64_t)d;
    }
    case T_STR: return tstr_prefix_int(((TStr *)v.u.p)->c, ((TStr *)v.u.p)->len);
    case T_ARR: return ((TArr *)v.u.p)->n != 0;
    default:
        tphp_raise("TypeError", "Unsupported operand types: value cannot be converted to int");
    }
}

double tval_to_float(TVal v) {
    switch (v.t) {
    case T_NULL: case T_FALSE: return 0.0;
    case T_TRUE: return 1.0;
    case T_INT: return (double)v.u.i;
    case T_FLOAT: return v.u.d;
    case T_STR: return tstr_prefix_float(((TStr *)v.u.p)->c, ((TStr *)v.u.p)->len);
    case T_ARR: return ((TArr *)v.u.p)->n != 0;
    default:
        tphp_raise("TypeError", "Unsupported operand types: value cannot be converted to float");
    }
}

TStr *tval_to_str(TVal v) {
    switch (v.t) {
    case T_NULL: return tstr_intern("", 0);
    case T_FALSE: return tstr_intern("", 0);
    case T_TRUE: return tstr_intern("1", 1);
    case T_INT: return tphp_int_to_str(v.u.i);
    case T_FLOAT: return tphp_float_to_str(v.u.d);
    case T_STR: {
        TStr *s = v.u.p;
        s->h.rc++;
        return s;
    }
    default:
        if (v.t == T_OBJ && tphp_tostr_hook)
            return tphp_tostr_hook(v);    /* __toString dispatch (or raise) */
        tphp_raise("TypeError",
                   "Value of type %s cannot be converted to string",
                   v.t == T_ARR ? "array" :
                   v.t == T_OBJ ? "object" :
                   v.t == T_CLOSURE ? "closure" : "unknown");
    }
}

/* ---- loose comparison, ported from zend_operators.c zend_compare ---- */
static int cmp_double(double a, double b) {
    if (isnan(a) || isnan(b)) return 1; /* PHP: NAN comparisons are unordered; == stays false */
    return a < b ? -1 : (a > b ? 1 : 0);
}

static int cmp_strings(TStr *a, TStr *b) {
    size_t la = a->len, lb = b->len;
    size_t min = la < lb ? la : lb;
    int r = min ? memcmp(a->c, b->c, min) : 0;
    if (r) return r < 0 ? -1 : 1;
    return la == lb ? 0 : (la < lb ? -1 : 1);
}

static int cmp_array(TArr *a, TArr *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    tarr_sync_buckets(a);   /* 反查键前补齐惰性桶（L5） */
    for (uint32_t i = 0; i < a->dused; i++) {
        TVal v = a->data[i];
        if (v.t == T_NULL) continue; /* free slot */
        /* find the key of this entry */
        TVal key = TV_NULL;
        bool found = false;
        for (uint32_t bk = 0; bk < a->bcap; bk++) {
            if (a->buckets[bk].val_off != UINT32_MAX && a->buckets[bk].val_off == i) {
                key = a->buckets[bk].key; found = true; break;
            }
        }
        if (!found) continue;
        TVal *bv = tarr_get(b, key);
        if (!bv) return 1; /* keys differ: PHP leaves order unspecified for <=> */
        int r = tval_compare(v, *bv);
        if (r) return r;
    }
    return 0;
}

static bool both_numeric_str(TStr *a, TStr *b, int *res) {
    bool ia, ib; int64_t va, vb; double da, db;
    if (!tstr_is_numeric(a->c, a->len, &ia, &va, &da)) return false;
    if (!tstr_is_numeric(b->c, b->len, &ib, &vb, &db)) return false;
    if (ia && ib) *res = va == vb ? 0 : (va < vb ? -1 : 1);
    else if (ia) *res = cmp_double((double)va, db);
    else if (ib) *res = cmp_double(da, (double)vb);
    else *res = cmp_double(da, db);
    return true;
}

int tval_compare(TVal a, TVal b) {
    bool abool = a.t == T_TRUE || a.t == T_FALSE;
    bool bbool = b.t == T_TRUE || b.t == T_FALSE;
    /* null vs string: null becomes "" */
    if (a.t == T_NULL && b.t == T_STR) return cmp_strings(tstr_intern("", 0), b.u.p);
    if (b.t == T_NULL && a.t == T_STR) return cmp_strings(a.u.p, tstr_intern("", 0));
    /* null / bool anywhere: compare as bools */
    if (a.t == T_NULL || b.t == T_NULL || abool || bbool) {
        bool x = tval_to_bool(a), y = tval_to_bool(b);
        return x == y ? 0 : (x ? 1 : -1);
    }
    if (a.t == T_ARR || b.t == T_ARR) {
        if (a.t == T_ARR && b.t == T_ARR) return cmp_array(a.u.p, b.u.p);
        return a.t == T_ARR ? 1 : -1; /* array is always greater than a scalar */
    }
    if (a.t == T_OBJ || b.t == T_OBJ) {
        /* PHP 8: object vs non-object is never equal (object ranks higher);
         * two objects compare recursively only when of the same class */
        if (a.t != T_OBJ || b.t != T_OBJ) return a.t == T_OBJ ? 1 : -1;
        GCHead *ha = a.u.p, *hb = b.u.p;
        if (ha->kind != GC_KIND_OBJ || hb->kind != GC_KIND_OBJ)
            return ha == hb ? 0 : 1;  /* exception objects: identity only */
        TObj *oa = (TObj *)ha, *ob = (TObj *)hb;
        if (oa->cls != ob->cls) return 1;
        for (int i = 0; i < oa->cls->n_props; i++) {
            TVal x = oa->props[i].t == T_UNDEF ? TV_NULL : oa->props[i];
            TVal y = ob->props[i].t == T_UNDEF ? TV_NULL : ob->props[i];
            if (tval_compare(x, y) != 0) return 1;
        }
        return 0;
    }
    if (a.t == T_STR && b.t == T_STR) {
        int r;
        if (both_numeric_str(a.u.p, b.u.p, &r)) return r;
        return cmp_strings(a.u.p, b.u.p);
    }
    /* string vs number */
    if (a.t == T_STR || b.t == T_STR) {
        TStr *s = (a.t == T_STR) ? a.u.p : b.u.p;
        bool is_int; int64_t iv; double dv;
        if (tstr_is_numeric(s->c, s->len, &is_int, &iv, &dv)) {
            if (a.t == T_STR) {
                if (b.t == T_INT && is_int) return iv == b.u.i ? 0 : (iv < b.u.i ? -1 : 1);
                double bx = tval_to_float(b);
                return cmp_double(is_int ? (double)iv : dv, bx);
            } else {
                if (a.t == T_INT && is_int) return a.u.i == iv ? 0 : (a.u.i < iv ? -1 : 1);
                double ax = tval_to_float(a);
                return cmp_double(ax, is_int ? (double)iv : dv);
            }
        }
        /* PHP 8: non-numeric string vs number -> number becomes string */
        if (a.t == T_STR) {
            TStr *bs = tval_to_str(b);
            int r = cmp_strings(s, bs);
            tval_release(&(TVal){ .u.p = bs, .t = T_STR });
            return r;
        } else {
            TStr *as = tval_to_str(a);
            int r = cmp_strings(as, b.u.p);
            tval_release(&(TVal){ .u.p = as, .t = T_STR });
            return r;
        }
    }
    /* pure numbers */
    if (a.t == T_INT && b.t == T_INT)
        return a.u.i == b.u.i ? 0 : (a.u.i < b.u.i ? -1 : 1);
    return cmp_double(tval_to_float(a), tval_to_float(b));
}

bool tval_loose_eq(TVal a, TVal b) {
    int r = tval_compare(a, b);
    return r == 0;
}

static bool strict_eq_val(TVal a, TVal b);

static bool strict_eq_arr(TArr *x, TArr *y) {
    if (x->n != y->n) return false;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < x->dused; i++) {
        if (x->data[i].t == T_NULL) continue;
        seen++;
    }
    if (seen != y->n) return false;
    /* same order, same keys, values strictly equal */
    uint32_t xi = 0, yi = 0;
    for (;;) {
        while (xi < x->dused && x->data[xi].t == T_NULL) xi++;
        while (yi < y->dused && y->data[yi].t == T_NULL) yi++;
        if (xi >= x->dused || yi >= y->dused) break;
        TVal kx = TV_NULL, ky = TV_NULL;
        for (uint32_t bk = 0; bk < x->bcap; bk++)
            if (x->buckets[bk].val_off == xi) { kx = x->buckets[bk].key; break; }
        for (uint32_t bk = 0; bk < y->bcap; bk++)
            if (y->buckets[bk].val_off == yi) { ky = y->buckets[bk].key; break; }
        if (!tval_strict_eq(kx, ky)) return false;
        if (!strict_eq_val(x->data[xi], y->data[yi])) return false;
        xi++; yi++;
    }
    return true;
}

static bool strict_eq_val(TVal a, TVal b) {
    if (a.t != b.t) {
        /* T_TRUE/T_FALSE are both "bool" */
        bool abool = a.t == T_TRUE || a.t == T_FALSE;
        bool bbool = b.t == T_TRUE || b.t == T_FALSE;
        if (!(abool && bbool)) return false;
    }
    switch (a.t) {
    case T_NULL: return b.t == T_NULL;
    case T_FALSE: return b.t == T_FALSE;
    case T_TRUE: return b.t == T_TRUE;
    case T_INT: return a.u.i == b.u.i;
    case T_FLOAT: return a.u.d == b.u.d;
    case T_STR: return tstr_eq(a.u.p, b.u.p);
    case T_ARR: return strict_eq_arr(a.u.p, b.u.p);
    default: return a.u.p == b.u.p;
    }
}

bool tval_strict_eq(TVal a, TVal b) { return strict_eq_val(a, b); }

/* ---- arithmetic ---- */

/* PHP arithmetic operand preparation: int/float pass through; strings must
 * be (leading-)numeric, anything else is a TypeError. */
typedef enum { AN_INT, AN_FLOAT } ANKind;
typedef struct { ANKind k; int64_t i; double d; } ANum;

static void to_num(TVal v, ANum *n, const char *op) {
    switch (v.t) {
    case T_INT: n->k = AN_INT; n->i = v.u.i; return;
    case T_FLOAT: n->k = AN_FLOAT; n->d = v.u.d; return;
    case T_STR: {
        TStr *s = v.u.p;
        bool is_int; int64_t iv; double dv;
        if (tstr_prefix_numeric(s->c, s->len, &is_int, &iv, &dv)) {
            if (is_int) { n->k = AN_INT; n->i = iv; }
            else { n->k = AN_FLOAT; n->d = dv; }
            return;
        }
        tphp_raise("TypeError", "Unsupported operand types: string %s number", op);
    }
    default:
        tphp_raise("TypeError", "Unsupported operand types in arithmetic");
    }
}

static TVal num_result_i(int64_t i) { return TV_INT(i); }
static TVal num_result_d(double d) { return TV_FLOAT(d); }

TVal tval_add(TVal a, TVal b) {
    ANum x, y; to_num(a, &x, "+"); to_num(b, &y, "+");
    if (x.k == AN_INT && y.k == AN_INT) {
        int64_t r;
        if (!__builtin_add_overflow(x.i, y.i, &r)) return num_result_i(r);
        return num_result_d((double)x.i + (double)y.i);
    }
    return num_result_d((x.k == AN_INT ? (double)x.i : x.d) + (y.k == AN_INT ? (double)y.i : y.d));
}

TVal tval_sub(TVal a, TVal b) {
    ANum x, y; to_num(a, &x, "-"); to_num(b, &y, "-");
    if (x.k == AN_INT && y.k == AN_INT) {
        int64_t r;
        if (!__builtin_sub_overflow(x.i, y.i, &r)) return num_result_i(r);
        return num_result_d((double)x.i - (double)y.i);
    }
    return num_result_d((x.k == AN_INT ? (double)x.i : x.d) - (y.k == AN_INT ? (double)y.i : y.d));
}

TVal tval_mul(TVal a, TVal b) {
    ANum x, y; to_num(a, &x, "*"); to_num(b, &y, "*");
    if (x.k == AN_INT && y.k == AN_INT) {
        int64_t r;
        if (!__builtin_mul_overflow(x.i, y.i, &r)) return num_result_i(r);
        return num_result_d((double)x.i * (double)y.i);
    }
    return num_result_d((x.k == AN_INT ? (double)x.i : x.d) * (y.k == AN_INT ? (double)y.i : y.d));
}

TVal tval_div(TVal a, TVal b) {
    ANum x, y; to_num(a, &x, "/"); to_num(b, &y, "/");
    double bd = y.k == AN_INT ? (double)y.i : y.d;
    if (bd == 0.0)
        tphp_raise("DivisionByZeroError", "Division by zero");
    if (x.k == AN_INT && y.k == AN_INT) {
        if (x.i % y.i == 0) return num_result_i(x.i / y.i);
    }
    double ad = x.k == AN_INT ? (double)x.i : x.d;
    return num_result_d(ad / bd);
}

TVal tval_mod(TVal a, TVal b) {
    int64_t x = tval_to_int(a), y = tval_to_int(b);
    if (y == 0)
        tphp_raise("DivisionByZeroError", "Modulo by zero");
    return num_result_i(x % y);
}

TVal tval_pow(TVal a, TVal b) {
    ANum x, y; to_num(a, &x, "**"); to_num(b, &y, "**");
    if (x.k == AN_INT && y.k == AN_INT && y.i >= 0) {
        int64_t base = x.i, acc = 1; int64_t e = y.i;
        bool ok = true;
        while (e > 0) {
            if (e & 1) { if (__builtin_mul_overflow(acc, base, &acc)) { ok = false; break; } }
            e >>= 1;
            if (e) { if (__builtin_mul_overflow(base, base, &base)) { ok = false; break; } }
        }
        if (ok) return num_result_i(acc);
        /* integer overflow: fall through to double math like PHP */
        return num_result_d(pow((double)x.i, (double)y.i));
    }
    double bd = y.k == AN_INT ? (double)y.i : y.d;
    double ad = x.k == AN_INT ? (double)x.i : x.d;
    return num_result_d(pow(ad, bd));
}

TVal tval_concat(TVal a, TVal b) {
    TStr *sa = tval_to_str(a);
    TStr *sb = tval_to_str(b);
    TStr *r = tstr_concat(sa, sb);
    tval_release(&(TVal){ .u.p = sa, .t = T_STR });
    tval_release(&(TVal){ .u.p = sb, .t = T_STR });
    return TV_STR(r);
}

static int64_t shift_amount(TVal v) {
    int64_t s = tval_to_int(v);
    if (s < 0)
        tphp_raise("ValueError", "Bit shift by negative number");
    return s;
}

TVal tval_shl(TVal a, TVal b) {
    int64_t x = tval_to_int(a), s = shift_amount(b);
    if (s >= 64) return num_result_i(0);
    return num_result_i((int64_t)((uint64_t)x << s));
}

TVal tval_shr(TVal a, TVal b) {
    int64_t x = tval_to_int(a), s = shift_amount(b);
    if (s >= 64) return num_result_i(x < 0 ? -1 : 0);
    return num_result_i(x >> s);
}

TVal tval_band(TVal a, TVal b) { return num_result_i(tval_to_int(a) & tval_to_int(b)); }
TVal tval_bor(TVal a, TVal b) { return num_result_i(tval_to_int(a) | tval_to_int(b)); }
TVal tval_bxor(TVal a, TVal b) { return num_result_i(tval_to_int(a) ^ tval_to_int(b)); }

TVal tval_bool_not(TVal v) { return tval_to_bool(v) ? TV_FALSE : TV_TRUE; }
TVal tval_neg(TVal v) {
    ANum x; to_num(v, &x, "-");
    if (x.k == AN_INT) {
        if (x.i == INT64_MIN) return num_result_d(-(double)x.i);
        return num_result_i(-x.i);
    }
    return num_result_d(-x.d);
}
TVal tval_pos(TVal v) {
    ANum x; to_num(v, &x, "+");
    return x.k == AN_INT ? num_result_i(x.i) : num_result_d(x.d);
}
TVal tval_bit_not(TVal v) { return num_result_i(~tval_to_int(v)); }

/* ---- string increment (PHP quirk), "a"->"b", "z"->"aa", "A1"->"A2" ---- */
static TStr *str_inc_alpha(TStr *s) {
    /* pure [A-Za-z0-9]+ carries: digits and letters increment like odometer */
    char *buf = tm_malloc(s->len + 2);
    memcpy(buf, s->c, s->len);
    size_t i = s->len;
    for (;;) {
        if (i == 0) {
            /* prepend carry */
            memmove(buf + 1, buf, s->len);
            buf[0] = isdigit((unsigned char)buf[1]) ? '1' : (isupper((unsigned char)buf[1]) ? 'A' : 'a');
            s->len++;
            TStr *r = tstr_new(buf, s->len);
            tm_free(buf);
            return r;
        }
        i--;
        unsigned char c = buf[i];
        if (c == '9') buf[i] = '0';
        else if (c == 'z') buf[i] = 'a';
        else if (c == 'Z') buf[i] = 'A';
        else if ((c >= '0' && c < '9') || (c >= 'a' && c < 'z') || (c >= 'A' && c < 'Z')) {
            buf[i] = (char)(c + 1);
            TStr *r = tstr_new(buf, s->len);
            tm_free(buf);
            return r;
        } else {
            /* non-alphanumeric character: PHP raises */
            tm_free(buf);
            tphp_raise("ValueError", "Cannot increment a non-numeric, non-alphanumeric string");
        }
    }
}

TVal tval_inc(TVal v) {
    switch (v.t) {
    case T_NULL: return TV_INT(1);
    case T_FALSE: return TV_INT(1);
    case T_TRUE: return TV_TRUE; /* bool++ is a no-op in PHP */
    case T_INT: return num_result_i(v.u.i + 1);
    case T_FLOAT: return num_result_d(v.u.d + 1.0);
    case T_STR: {
        TStr *s = v.u.p;
        bool is_int; int64_t iv; double dv;
        if (s->len && tstr_is_numeric(s->c, s->len, &is_int, &iv, &dv)) {
            if (is_int) return num_result_i(iv + 1);
            return num_result_d(dv + 1.0);
        }
        /* alphanumeric increment */
        bool alnum = s->len > 0;
        for (size_t i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->c[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) { alnum = false; break; }
        }
        if (alnum) return TV_STR(str_inc_alpha(s));
        tphp_raise("ValueError", "Cannot increment a non-numeric string");
    }
    default:
        tphp_raise("ValueError", "Unsupported operand types in increment");
    }
}

TVal tval_dec(TVal v) {
    switch (v.t) {
    case T_NULL: return TV_NULL; /* NULL-- stays NULL in PHP */
    case T_FALSE: return TV_FALSE; /* bool-- is a no-op in PHP */
    case T_TRUE: return TV_TRUE;
    case T_INT: return num_result_i(v.u.i - 1);
    case T_FLOAT: return num_result_d(v.u.d - 1.0);
    case T_STR: {
        TStr *s = v.u.p;
        bool is_int; int64_t iv; double dv;
        if (s->len && tstr_is_numeric(s->c, s->len, &is_int, &iv, &dv)) {
            if (is_int) return num_result_i(iv - 1);
            return num_result_d(dv - 1.0);
        }
        tphp_raise("ValueError", "Cannot decrement a non-numeric string");
    }
    default:
        tphp_raise("ValueError", "Unsupported operand types in decrement");
    }
}

/* ---- array key normalization ---- */
bool tval_norm_key(TVal *key) {
    switch (key->t) {
    case T_INT: return true;
    case T_FALSE: *key = TV_INT(0); return true;
    case T_TRUE: *key = TV_INT(1); return true;
    case T_NULL: *key = TV_STR(tstr_intern("", 0)); return true;
    case T_FLOAT: {
        double d = key->u.d;
        if (isnan(d))
            tphp_raise("ValueError", "NAN cannot be used as an array key");
        if (d >= 9223372036854775808.0 || d <= -9223372036854775808.0)
            tphp_raise("ValueError", "Cannot convert float to int, value out of range");
        *key = TV_INT((int64_t)d);
        return true;
    }
    case T_STR: {
        TStr *s = key->u.p;
        /* interned non-numeric key: skip digit scan (P0-3 hash/norm cache) */
        if (s->h.flags & GC_FLAG_NONNUM_KEY) return true;
        /* canonical decimal integer string -> int key (PHP rule: "8" -> 8,
         * "08" stays string, "-3" -> -3, " 8" stays string) */
        size_t i = 0;
        bool neg = false;
        if (s->len && s->c[0] == '-') { neg = true; i = 1; }
        if (i < s->len) {
            bool all_digits = true;
            for (size_t k = i; k < s->len; k++)
                if (s->c[k] < '0' || s->c[k] > '9') { all_digits = false; break; }
            if (all_digits) {
                bool leading_zero = (s->len - i > 1 && s->c[i] == '0');
                if (!leading_zero) {
                    uint64_t m = 0; bool ovf = false;
                    for (size_t k = i; k < s->len; k++) {
                        unsigned d = (unsigned)(s->c[k] - '0');
                        if (m > (UINT64_MAX - d) / 10) { ovf = true; break; }
                        m = m * 10 + d;
                    }
                    if (!ovf) {
                        if (neg && m == (uint64_t)INT64_MAX + 1) *key = TV_INT(INT64_MIN);
                        else if (neg && m > (uint64_t)INT64_MAX) { /* overflow: stays string */ }
                        else *key = TV_INT(neg ? -(int64_t)m : (int64_t)m);
                    }
                }
            }
        }
        return true;
    }
    default:
        return false; /* objects etc. are illegal keys in M1 */
    }
}
