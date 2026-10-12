/*
 * TinyPHP.rt - a small PHP runtime
 * builtin.c - M2 built-in functions + the print/echo core + var_dump
 *
 * PHP 8 alignment: type errors raise TypeError (via the VM exception
 * machinery); str_repeat/count/strlen/intdiv check argument types strictly.
 */
#include "value.h"
#include "opcode.h"
#include "tphp.h"
#include "class.h"
#include "sched.h"
#include "tio.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

TArr *tarr_new(void);

#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)

/* ---- output core (echo/print) ---- */
void tphp_echo_val(TVal v) {
    TStr *s = tval_to_str(v);
    fwrite(tstr_val(s), 1, tstr_len(s), stdout);
    tval_release(&(TVal){ .u.p = s, .t = T_STR });
}

/* ---- var_dump ---- */
static void dump_pad(int n) {
    for (int i = 0; i < n; i++) putchar(' ');
}

static TVal arr_key_at(TArr *a, uint32_t di) {
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t != T_NULL && bk->val_off == di)
            return bk->key;
    }
    return TV_NULL;
}

static void dump_val(TVal v, int depth);

/* var_dump array body (shared with __debugInfo rendering): entries at the
 * given depth, already inside the braces */
static void dump_array_body(TArr *a, int depth) {
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal k = arr_key_at(a, i);
        dump_pad(depth * 2 + 2);
        if (k.t == T_INT) printf("[%lld]=>\n", (long long)k.u.i);
        else {
            TStr *ks = (TStr *)k.u.p;
            printf("[\"%s\"]=>\n", tstr_val(ks));
        }
        dump_val(a->data[i], depth + 1);
    }
}

static void dump_array(TArr *a, int depth) {
    dump_pad(depth * 2);
    printf("array(%u) {\n", a->n);
    dump_array_body(a, depth);
    dump_pad(depth * 2);
    printf("}\n");
}

static void dump_val(TVal v, int depth) {
    if (v.t != T_ARR) dump_pad(depth * 2);   /* arrays pad inside dump_array */
    switch (v.t) {
    case T_NULL: printf("NULL\n"); return;
    case T_FALSE: printf("bool(false)\n"); return;
    case T_TRUE: printf("bool(true)\n"); return;
    case T_INT: printf("int(%lld)\n", (long long)v.u.i); return;
    case T_FLOAT: {
        TStr *s = tphp_float_to_str(v.u.d);
        printf("float(%s)\n", tstr_val(s));
        tval_release(&(TVal){ .u.p = s, .t = T_STR });
        return;
    }
    case T_STR: {
        TStr *s = (TStr *)v.u.p;
        printf("string(%u) \"%s\"\n", s->len, tstr_val(s));
        return;
    }
    case T_ARR: dump_array((TArr *)v.u.p, depth); return;
    case T_OBJ: {
        GCHead *h = (GCHead *)v.u.p;
        if (h->kind == GC_KIND_OBJ) {
            TObj *o = (TObj *)h;
            if (o->cls->kind == CK_ENUM) {
                /* PHP 8 enum cases dump as enum(Suit::Hearts) */
                const char *cn = tstr_val(o->cls->name);
                TStr *case_name = NULL;
                int ci = tphp_enum_case_find(o->cls, o);
                if (ci >= 0) case_name = o->cls->cases[ci].name;
                if (case_name)
                    printf("enum(%s::%s)\n", cn, tstr_val(case_name));
                else
                    printf("enum(%s)\n", cn);
                return;
            }
            /* __debugInfo overrides the rendered properties */
            TVal di = tphp_vm_debug_info(o);
            if (di.t == T_ARR) {
                TArr *a = (TArr *)di.u.p;
                printf("object(%s)#%u (%u) {\n", tstr_val(o->cls->name),
                       o->oid, a->n);
                dump_array_body(a, depth);
                dump_pad(depth * 2);
                printf("}\n");
                tval_release(&di);
                return;
            }
            uint32_t n = 0;
            for (int i = 0; i < o->cls->n_props; i++)
                if (o->props[i].t != T_UNDEF) n++;
            printf("object(%s)#%u (%u) {\n", tstr_val(o->cls->name), o->oid, n);
            for (int i = 0; i < o->cls->n_props; i++) {
                if (o->props[i].t == T_UNDEF) continue;
                dump_pad(depth * 2 + 2);
                printf("[\"%s\"]=>\n", tstr_val(o->cls->props[i].name));
                dump_val(o->props[i], depth + 1);
            }
            dump_pad(depth * 2);
            printf("}\n");
            return;
        }
        /* built-in exception object */
        TExObj *x = (TExObj *)h;
        printf("object(%s)#%u (%d) {\n", x->cls->name, 0, 1);
        dump_pad(depth * 2 + 2);
        printf("[\"message\"]=>\n");
        dump_val(TV_STR(x->message), depth + 1);
        dump_pad(depth * 2);
        printf("}\n");
        return;
    }
    default: printf("unknown\n"); return;
    }
}

static TVal bi_var_dump(VM *vm, TVal *args, int n) {
    (void)vm;
    for (int i = 0; i < n; i++)
        dump_val(args[i], 0);
    return TV_NULL;
}

/* ---- serialize / unserialize (M5-b) ---- */
static TVal bi_serialize(VM *vm, TVal *args, int n) {
    if (getenv("TPHP_SER_DEBUG"))
        fprintf(stderr, "[bi-serialize] n=%d args0.t=%d\n", n,
                n > 0 ? (int)args[0].t : -1);
    if (n < 1)
        RAISE(vm, "ArgumentCountError",
              "serialize() expects at least 1 argument");
    return TV_STR(tphp_serialize_val(vm, args[0]));
}

static TVal bi_unserialize(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "unserialize() expects a string");
    TStr *s = (TStr *)args[0].u.p;
    TVal out;
    if (!tphp_unserialize_val(vm, tstr_val(s), s->len, &out)) {
        fprintf(stderr, "PHP Warning: unserialize(): Error at offset 0\n");
        return TV_FALSE;
    }
    return out;
}

/* ---- print_r ---- */
static void pr_append(char **out, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        *cap = (*len + n + 1) * 2;
        *out = tm_realloc(*out, *cap);
    }
    memcpy(*out + *len, s, n);
    *len += n;
}
static void pr_cstr(char **out, size_t *len, size_t *cap, const char *s) {
    pr_append(out, len, cap, s, strlen(s));
}

static void pr_val(TVal v, int depth, char **out, size_t *len, size_t *cap) {
    char buf[64];
    switch (v.t) {
    case T_NULL: case T_FALSE:
        return;
    case T_TRUE:
        pr_cstr(out, len, cap, "1");
        return;
    case T_INT:
        snprintf(buf, sizeof buf, "%lld", (long long)v.u.i);
        pr_cstr(out, len, cap, buf);
        return;
    case T_FLOAT: {
        TStr *s = tphp_float_to_str(v.u.d);
        pr_append(out, len, cap, tstr_val(s), tstr_len(s));
        tval_release(&(TVal){ .u.p = s, .t = T_STR });
        return;
    }
    case T_STR: {
        TStr *s = (TStr *)v.u.p;
        pr_append(out, len, cap, tstr_val(s), tstr_len(s));
        return;
    }
    case T_ARR: {
        TArr *a = (TArr *)v.u.p;
        pr_cstr(out, len, cap, "Array\n");
        for (int i = 0; i < depth * 4 + 8; i++) pr_cstr(out, len, cap, " ");
        pr_cstr(out, len, cap, "(\n");
        for (uint32_t i = 0; i < a->dused; i++) {
            if (a->data[i].t == T_UNDEF) continue;
            TVal k = arr_key_at(a, i);
            for (int s2 = 0; s2 < depth * 4 + 8; s2++) pr_cstr(out, len, cap, " ");
            pr_cstr(out, len, cap, "[");
            if (k.t == T_INT) {
                snprintf(buf, sizeof buf, "%lld", (long long)k.u.i);
                pr_cstr(out, len, cap, buf);
            } else {
                pr_cstr(out, len, cap, tstr_val((TStr *)k.u.p));
            }
            pr_cstr(out, len, cap, "] => ");
            pr_val(a->data[i], depth + 1, out, len, cap);
            pr_cstr(out, len, cap, "\n");
        }
        for (int i = 0; i < depth * 4 + 8; i++) pr_cstr(out, len, cap, " ");
        pr_cstr(out, len, cap, ")\n");
        return;
    }
    default: return;
    }
}

static TVal bi_print_r(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "print_r() expects at least 1 argument");
    char *out = NULL;
    size_t len = 0, cap = 0;
    pr_val(args[0], 0, &out, &len, &cap);
    if (n >= 2 && tval_to_bool(args[1])) {
        TStr *s = tstr_new(out ? out : "", len);
        if (out) tm_free(out);
        return TV_STR(s);
    }
    if (out) fwrite(out, 1, len, stdout);
    if (out) tm_free(out);
    return TV_TRUE;
}

/* ---- type inspection ---- */
static const char *type_name(TType t) {
    switch (t) {
    case T_NULL: return "NULL";
    case T_FALSE: case T_TRUE: return "boolean";
    case T_INT: return "integer";
    case T_FLOAT: return "double";
    case T_STR: return "string";
    case T_ARR: return "array";
    default: return "unknown";
    }
}

static TVal bi_gettype(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 1) RAISE(vm, "ArgumentCountError", "gettype() expects exactly 1 argument");
    const char *t = type_name(args[0].t);
    return TV_STR(tstr_intern(t, strlen(t)));
}

/* get_class: dispatch on the GC kind (user TObj vs built-in TExObj) */
static TVal bi_get_class(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 1) RAISE(vm, "ArgumentCountError", "get_class() expects exactly 1 argument");
    if (args[0].t != T_OBJ)
        RAISE(vm, "TypeError", "get_class() expects an object");
    GCHead *h = (GCHead *)args[0].u.p;
    if (h->kind == GC_KIND_OBJ) {          /* user class instance */
        TObj *o = (TObj *)h;
        return TV_STR(o->cls->name);
    }
    TExObj *x = (TExObj *)h;               /* built-in exception object */
    return TV_STR(tstr_intern(x->cls->name, strlen(x->cls->name)));
}

/* ---- runtime constants (same table as OP_DEFINE_CONST/OP_FETCH_CONST) ---- */
static TVal bi_define(VM *vm, TVal *args, int n) {
    if (n != 2) RAISE(vm, "ArgumentCountError", "define() expects exactly 2 arguments");
    if (args[0].t != T_STR)
        RAISE(vm, "TypeError", "define(): Argument #1 ($constant_name) must be a string");
    TStr *name = (TStr *)args[0].u.p;
    for (int i = 0; i < vm->n_consts; i++)
        if (tstr_eq(vm->consts[i].name, name))
            return TV_FALSE;               /* PHP 8: redefinition warns, returns false */
    if (vm->n_consts >= 256)
        RAISE(vm, "Error", "Too many constants");
    TVal v = args[1];
    if (v.t == T_ARR) tval_addref(v);      /* arrays are heap values */
    vm->consts[vm->n_consts].name = name;
    vm->consts[vm->n_consts].v = v;
    vm->n_consts++;
    return TV_TRUE;
}

static TVal bi_defined(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 1) RAISE(vm, "ArgumentCountError", "defined() expects exactly 1 argument");
    if (args[0].t != T_STR)
        RAISE(vm, "TypeError", "defined(): Argument #1 ($constant_name) must be a string");
    TStr *name = (TStr *)args[0].u.p;
    for (int i = 0; i < vm->n_consts; i++)
        if (tstr_eq(vm->consts[i].name, name))
            return TV_TRUE;
    return TV_FALSE;
}

/* ---- autoload / class inspection ---- */
static TVal bi_spl_autoload_register(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 1)
        RAISE(vm, "ArgumentCountError", "spl_autoload_register() expects exactly 1 argument");
    TVal fn = args[0];
    if (fn.t != T_CLOSURE && fn.t != T_STR)
        RAISE(vm, "TypeError",
              "spl_autoload_register(): Argument #1 ($callback) must be a callable");
    tphp_class_set_autoload_val(fn);
    return TV_TRUE;
}

static TVal bi_class_exists(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1 || n > 2)
        RAISE(vm, "ArgumentCountError", "class_exists() expects 1 or 2 arguments");
    if (args[0].t != T_STR)
        RAISE(vm, "TypeError", "class_exists(): Argument #1 ($class) must be a string");
    TStr *name = (TStr *)args[0].u.p;
    char lc[256];
    const char *s = tstr_val(name);
    size_t k = 0;
    for (; s[k] && k < sizeof lc - 1; k++)
        lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
    lc[k] = 0;
    extern TClass *class_resolve_lc(const char *fqcn_lc);
    return class_resolve_lc(lc) ? TV_TRUE : TV_FALSE;
}

#define PRED1(name, test) \
    static TVal name(VM *vm, TVal *args, int n) { \
        (void)vm; (void)n; \
        return (test) ? TV_TRUE : TV_FALSE; \
    }

PRED1(bi_is_int, args[0].t == T_INT)
PRED1(bi_is_float, args[0].t == T_FLOAT)
PRED1(bi_is_string, args[0].t == T_STR)
PRED1(bi_is_array, args[0].t == T_ARR)
PRED1(bi_is_bool, args[0].t == T_TRUE || args[0].t == T_FALSE)
PRED1(bi_is_null, args[0].t == T_NULL)

static TVal bi_is_numeric(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    if (args[0].t != T_STR)
        return (args[0].t == T_INT || args[0].t == T_FLOAT) ? TV_TRUE : TV_FALSE;
    TStr *s = (TStr *)args[0].u.p;
    bool is_int; int64_t iv; double dv;
    return tstr_is_numeric(tstr_val(s), s->len, &is_int, &iv, &dv) ? TV_TRUE : TV_FALSE;
}

/* ---- scalar functions ---- */
static TVal bi_print(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 1) RAISE(vm, "ArgumentCountError", "print() expects exactly 1 argument");
    tphp_echo_val(args[0]);
    return TV_INT(1);
}

static TArr *need_arr(VM *vm, TVal *v, const char *fn, const char *arg) {
    if (v->t != T_ARR)
        RAISE(vm, "TypeError", "%s(): Argument %s must be of type array, %s given",
              fn, arg, type_name(v->t));
    return (TArr *)v->u.p;
}

static TStr *need_str(VM *vm, TVal *v, const char *fn, const char *arg) {
    if (v->t != T_STR)
        RAISE(vm, "TypeError", "%s(): Argument %s must be of type string, %s given",
              fn, arg, type_name(v->t));
    return (TStr *)v->u.p;
}

static int64_t need_int(VM *vm, TVal *v, const char *fn, const char *arg) {
    if (v->t != T_INT)
        RAISE(vm, "TypeError", "%s(): Argument %s must be of type int, %s given",
              fn, arg, type_name(v->t));
    return v->u.i;
}

static TVal bi_count(VM *vm, TVal *args, int n) {
    if (n < 1 || n > 2) RAISE(vm, "ArgumentCountError", "count() expects 1 or 2 arguments");
    need_arr(vm, &args[0], "count", "#1 ($value)");
    return TV_INT((int64_t)((TArr *)args[0].u.p)->n);
}

static TVal bi_strlen(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "strlen() expects exactly 1 argument");
    TStr *s = need_str(vm, &args[0], "strlen", "#1 ($string)");
    return TV_INT((int64_t)s->len);
}

static TVal bi_abs(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 1) RAISE(vm, "ArgumentCountError", "abs() expects exactly 1 argument");
    TVal v = args[0];
    if (v.t == T_INT)
        return v.u.i == INT64_MIN ? TV_FLOAT(9223372036854775808.0)
                                  : TV_INT(v.u.i < 0 ? -v.u.i : v.u.i);
    if (v.t == T_FLOAT) return TV_FLOAT(v.u.d < 0 ? -v.u.d : v.u.d);
    if (v.t == T_FALSE || v.t == T_NULL) return TV_INT(0);
    if (v.t == T_TRUE) return TV_INT(1);
    if (v.t == T_STR) {
        TStr *s = (TStr *)v.u.p;
        bool is_int; int64_t iv; double dv;
        if (tstr_prefix_numeric(tstr_val(s), s->len, &is_int, &iv, &dv))
            return is_int ? TV_INT(iv < 0 ? -iv : iv) : TV_FLOAT(dv < 0 ? -dv : dv);
        RAISE(vm, "TypeError", "abs(): Argument #1 ($num) must be of type int|float, non-numeric string given");
    }
    RAISE(vm, "TypeError", "Unsupported operand types in abs()");
}

static TVal extreme(VM *vm, TVal *args, int n, bool want_max) {
    if (n == 0) RAISE(vm, "ArgumentCountError", "at least 1 argument expected");
    TVal best;
    int count;
    if (n == 1 && args[0].t == T_ARR) {
        TArr *a = (TArr *)args[0].u.p;
        if (a->n == 0)
            RAISE(vm, "ValueError", "Array must contain at least one element");
        best = TV_NULL; count = 0;
        for (uint32_t i = 0; i < a->dused; i++) {
            if (a->data[i].t == T_UNDEF) continue;
            if (count == 0) best = a->data[i];
            else {
                int cmp = tval_compare(a->data[i], best);
                if (want_max ? cmp > 0 : cmp < 0) best = a->data[i];
            }
            count++;
        }
        return best;
    }
    best = args[0];
    for (int i = 1; i < n; i++) {
        int cmp = tval_compare(args[i], best);
        if (want_max ? cmp > 0 : cmp < 0) best = args[i];
    }
    return best;
}

static TVal bi_max(VM *vm, TVal *args, int n) { return extreme(vm, args, n, true); }
static TVal bi_min(VM *vm, TVal *args, int n) { return extreme(vm, args, n, false); }

static TVal bi_intdiv(VM *vm, TVal *args, int n) {
    if (n != 2) RAISE(vm, "ArgumentCountError", "intdiv() expects exactly 2 arguments");
    int64_t a = need_int(vm, &args[0], "intdiv", "#1 ($num1)");
    int64_t b = need_int(vm, &args[1], "intdiv", "#2 ($num2)");
    if (b == 0) RAISE(vm, "DivisionByZeroError", "Division by zero");
    if (a == INT64_MIN && b == -1)
        RAISE(vm, "Error", "Division of PHP_INT_MIN by -1 is not an integer");
    return TV_INT(a / b);
}

static TVal bi_pow(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 2) RAISE(vm, "ArgumentCountError", "pow() expects exactly 2 arguments");
    return tval_pow(args[0], args[1]);
}

static TVal bi_gc_collect_cycles(VM *vm, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return TV_INT(gc_collect());
}

static TVal bi_str_repeat(VM *vm, TVal *args, int n) {
    if (n != 2) RAISE(vm, "ArgumentCountError", "str_repeat() expects exactly 2 arguments");
    TStr *s = need_str(vm, &args[0], "str_repeat", "#1 ($string)");
    int64_t times = need_int(vm, &args[1], "str_repeat", "#2 ($times)");
    if (times < 0)
        RAISE(vm, "ValueError", "str_repeat(): Argument #2 ($times) must be greater than or equal to 0");
    if (times == 0 || s->len == 0)
        return TV_STR(tstr_new("", 0));
    int64_t total = s->len * times;
    if (total > 256 * 1024 * 1024)
        RAISE(vm, "ValueError", "str_repeat(): result too large");
    TStr *out = tstr_alloc_raw((uint32_t)total, false);
    for (int64_t i = 0; i < times; i++)
        memcpy(out->c + i * s->len, tstr_val(s), s->len);
    out->c[total] = 0;
    return TV_STR(out);
}

static TVal bi_exit(VM *vm, TVal *args, int n) {
    (void)vm;
    int code = 0;
    if (n > 0) {
        if (args[0].t == T_INT) code = (int)args[0].u.i;
        else tphp_echo_val(args[0]);
    }
    fflush(stdout);
    exit(code);
}

/* ---- conversions ---- */
static TVal bi_intval(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "intval() expects at least 1 argument");
    if (args[0].t == T_INT) return args[0];
    return TV_INT(tval_to_int(args[0]));
}

static TVal bi_floatval(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "floatval() expects at least 1 argument");
    if (args[0].t == T_FLOAT) return args[0];
    return TV_FLOAT(tval_to_float(args[0]));
}

static TVal bi_strval(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "strval() expects at least 1 argument");
    return TV_STR(tval_to_str(args[0]));
}

static TVal bi_boolval(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "boolval() expects at least 1 argument");
    return tval_to_bool(args[0]) ? TV_TRUE : TV_FALSE;
}

/* ---- arrays ---- */
static TVal bi_array_key_exists(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n != 2) RAISE(vm, "ArgumentCountError", "array_key_exists() expects exactly 2 arguments");
    TArr *a = need_arr(vm, &args[1], "array_key_exists", "#2 ($array)");
    TVal k = args[0];
    if (!tval_norm_key(&k)) return TV_FALSE;
    return tarr_has(a, k) ? TV_TRUE : TV_FALSE;
}

static TVal bi_in_array(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2) RAISE(vm, "ArgumentCountError", "in_array() expects at least 2 arguments");
    TArr *a = need_arr(vm, &args[1], "in_array", "#2 ($haystack)");
    bool strict = n >= 3 && tval_to_bool(args[2]);
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        if (strict ? tval_strict_eq(args[0], a->data[i])
                   : tval_loose_eq(args[0], a->data[i]))
            return TV_TRUE;
    }
    return TV_FALSE;
}

static TVal bi_array_push(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError", "array_push() expects at least 2 arguments");
    TArr *a = need_arr(vm, &args[0], "array_push", "#1 ($array)");
    for (int i = 1; i < n; i++) {
        tval_addref(args[i]);
        tarr_append(a, args[i]);
    }
    return TV_INT((int64_t)a->n);
}

static TVal bi_array_pop(VM *vm, TVal *args, int n) {
    (void)n;
    TArr *a = need_arr(vm, &args[0], "array_pop", "#1 ($array)");
    if (a->n == 0) return TV_NULL;
    for (int64_t i = (int64_t)a->dused - 1; i >= 0; i--) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal v = a->data[i];
        TVal key = arr_key_at(a, (uint32_t)i);
        tarr_unset(a, key);
        return v;   /* ownership moves out */
    }
    return TV_NULL;
}

static TVal bi_array_shift(VM *vm, TVal *args, int n) {
    (void)n;
    TArr *a = need_arr(vm, &args[0], "array_shift", "#1 ($array)");
    if (a->n == 0) return TV_NULL;
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal v = a->data[i];
        TVal key = arr_key_at(a, i);
        tarr_unset(a, key);
        return v;
    }
    return TV_NULL;
}

static TVal bi_array_unshift(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError", "array_unshift() expects at least 2 arguments");
    TArr *a = need_arr(vm, &args[0], "array_unshift", "#1 ($array)");
    TArr *b = tarr_new();
    for (int i = 1; i < n; i++) {
        tval_addref(args[i]);
        tarr_append(b, args[i]);
    }
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal k = arr_key_at(a, i);
        TVal v = a->data[i];
        tval_addref(v);
        if (k.t == T_STR) tarr_set(b, k, v);
        else tarr_append(b, v);
    }
    tval_release(&args[0]);
    args[0] = TV_ARR(b);
    return TV_INT((int64_t)b->n);
}

static TArr *copy_arr(TArr *a) {
    TArr *b = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal k = arr_key_at(a, i);
        TVal v = a->data[i];
        tval_addref(v);
        if (k.t == T_STR) tarr_set(b, k, v);
        else tarr_append(b, v);
    }
    return b;
}

static TVal bi_array_keys(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TArr *a = need_arr(vm, &args[0], "array_keys", "#1 ($array)");
    TArr *b = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal k = arr_key_at(a, i);
        tarr_append(b, k);
    }
    return TV_ARR(b);
}

static TVal bi_array_values(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TArr *a = need_arr(vm, &args[0], "array_values", "#1 ($array)");
    TArr *b = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal v = a->data[i];
        tval_addref(v);
        tarr_append(b, v);
    }
    return TV_ARR(b);
}

static TVal bi_array_merge(VM *vm, TVal *args, int n) {
    if (n < 1) RAISE(vm, "ArgumentCountError", "array_merge() expects at least 1 argument");
    TArr *b = tarr_new();
    for (int ai = 0; ai < n; ai++) {
        TArr *a = need_arr(vm, &args[ai], "array_merge", "argument");
        for (uint32_t i = 0; i < a->dused; i++) {
            if (a->data[i].t == T_UNDEF) continue;
            TVal k = arr_key_at(a, i);
            TVal v = a->data[i];
            tval_addref(v);
            if (k.t == T_STR) tarr_set(b, k, v);
            else tarr_append(b, v);   /* int keys re-indexed (PHP merge) */
        }
    }
    return TV_ARR(b);
}

#define MAX_SEQ 4096

static TVal bi_array_slice(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2) RAISE(vm, "ArgumentCountError", "array_slice() expects at least 2 arguments");
    TArr *a = need_arr(vm, &args[0], "array_slice", "#1 ($array)");
    int64_t offset = need_int(vm, &args[1], "array_slice", "#2 ($offset)");
    int64_t len = n >= 3 ? need_int(vm, &args[2], "array_slice", "#3 ($length)") : INT64_MAX;
    uint32_t idx[MAX_SEQ]; uint32_t cnt = 0;
    for (uint32_t i = 0; i < a->dused && cnt < MAX_SEQ; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        idx[cnt++] = i;
    }
    if (offset < 0) offset += (int64_t)cnt;
    if (offset < 0) offset = 0;
    if (offset > (int64_t)cnt) offset = (int64_t)cnt;
    int64_t end = len < 0 ? (int64_t)cnt + len : offset + len;
    if (end > (int64_t)cnt) end = (int64_t)cnt;
    if (end < offset) end = offset;
    TArr *b = tarr_new();
    for (uint32_t j = 0; j < cnt; j++) {
        if ((int64_t)j < offset || (int64_t)j >= end) continue;
        TVal k = arr_key_at(a, idx[j]);
        TVal v = a->data[idx[j]];
        tval_addref(v);
        if (k.t == T_STR) tarr_set(b, k, v);
        else tarr_append(b, v);
    }
    return TV_ARR(b);
}

static TVal bi_array_reverse(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TArr *a = need_arr(vm, &args[0], "array_reverse", "#1 ($array)");
    uint32_t idx[MAX_SEQ]; uint32_t cnt = 0;
    for (uint32_t i = 0; i < a->dused && cnt < MAX_SEQ; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        idx[cnt++] = i;
    }
    TArr *b = tarr_new();
    for (uint32_t j = cnt; j-- > 0; ) {
        TVal k = arr_key_at(a, idx[j]);
        TVal v = a->data[idx[j]];
        tval_addref(v);
        if (k.t == T_STR) tarr_set(b, k, v);
        else tarr_append(b, v);
    }
    return TV_ARR(b);
}

static TVal bi_array_sum(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TArr *a = need_arr(vm, &args[0], "array_sum", "#1 ($array)");
    bool any_float = false;
    int64_t si = 0;
    double sd = 0;
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal v = a->data[i];
        if (v.t == T_INT) { si += v.u.i; if (any_float) sd += (double)v.u.i; }
        else if (v.t == T_FLOAT) {
            if (!any_float) { sd = (double)si; any_float = true; }
            sd += v.u.d;
        }
        else if (v.t == T_STR) {
            TStr *s = (TStr *)v.u.p;
            bool is_int; int64_t iv; double dv;
            if (tstr_prefix_numeric(tstr_val(s), s->len, &is_int, &iv, &dv)) {
                if (is_int) { si += iv; if (any_float) sd += (double)iv; }
                else {
                    if (!any_float) { sd = (double)si; any_float = true; }
                    sd += dv;
                }
            }
        } else if (v.t == T_TRUE) { si += 1; if (any_float) sd += 1; }
        else if (v.t == T_FALSE || v.t == T_NULL) { }
        else RAISE(vm, "TypeError", "Unsupported operand types in array_sum()");
    }
    return any_float ? TV_FLOAT(sd) : TV_INT(si);
}

static int loose_cmp(TVal a, TVal b) { return tval_compare(a, b); }

static TVal bi_sort(VM *vm, TVal *args, int n) {
    (void)n;
    TArr *a = need_arr(vm, &args[0], "sort", "#1 ($array)");
    TVal vals[MAX_SEQ]; uint32_t cnt = 0;
    for (uint32_t i = 0; i < a->dused && cnt < MAX_SEQ; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        vals[cnt++] = a->data[i];
    }
    for (uint32_t i = 1; i < cnt; i++) {
        TVal key = vals[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && loose_cmp(vals[j], key) > 0) { vals[j + 1] = vals[j]; j--; }
        vals[j + 1] = key;
    }
    TArr *b = tarr_new();
    for (uint32_t i = 0; i < cnt; i++) {
        tval_addref(vals[i]);
        tarr_append(b, vals[i]);
    }
    tval_release(&args[0]);
    args[0] = TV_ARR(b);
    return TV_TRUE;
}

static TVal bi_rsort(VM *vm, TVal *args, int n) {
    (void)n;
    TArr *a = need_arr(vm, &args[0], "rsort", "#1 ($array)");
    TVal vals[MAX_SEQ]; uint32_t cnt = 0;
    for (uint32_t i = 0; i < a->dused && cnt < MAX_SEQ; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        vals[cnt++] = a->data[i];
    }
    for (uint32_t i = 1; i < cnt; i++) {
        TVal key = vals[i];
        int64_t j = (int64_t)i - 1;
        while (j >= 0 && loose_cmp(vals[j], key) < 0) { vals[j + 1] = vals[j]; j--; }
        vals[j + 1] = key;
    }
    TArr *b = tarr_new();
    for (uint32_t i = 0; i < cnt; i++) {
        tval_addref(vals[i]);
        tarr_append(b, vals[i]);
    }
    tval_release(&args[0]);
    args[0] = TV_ARR(b);
    return TV_TRUE;
}

/* ---- strings ---- */
static TVal bi_substr(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError", "substr() expects at least 2 arguments");
    TStr *s = need_str(vm, &args[0], "substr", "#1 ($string)");
    int64_t start = need_int(vm, &args[1], "substr", "#2 ($offset)");
    int64_t slen = (int64_t)s->len;
    if (start < 0) start += slen;
    if (start < 0) start = 0;
    if (start > slen) return TV_STR(tstr_new("", 0));
    int64_t end = slen;
    if (n >= 3) {
        int64_t len = need_int(vm, &args[2], "substr", "#3 ($length)");
        if (len < 0) end = slen + len;
        else end = start + len;
    }
    if (end < start) end = start;
    if (end > slen) end = slen;
    if (end == start) return TV_STR(tstr_new("", 0));
    return TV_STR(tstr_new(s->c + start, (size_t)(end - start)));
}

static TVal bi_str_replace(VM *vm, TVal *args, int n) {
    if (n < 3) RAISE(vm, "ArgumentCountError", "str_replace() expects at least 3 arguments");
    TStr *search = need_str(vm, &args[0], "str_replace", "#1 ($search)");
    TStr *replace = need_str(vm, &args[1], "str_replace", "#2 ($replace)");
    TStr *subject = need_str(vm, &args[2], "str_replace", "#3 ($subject)");
    if (search->len == 0) return args[2];
    char *buf = NULL; size_t cap = 0, len = 0;
    const char *p = tstr_val(subject);
    const char *end = p + subject->len;
    const char *nd = tstr_val(search);
    while (p < end) {
        if ((size_t)(end - p) >= search->len && memcmp(p, nd, search->len) == 0) {
            if (len + replace->len + 1 > cap) {
                cap = (len + replace->len + 1) * 2;
                buf = tm_realloc(buf, cap);
            }
            memcpy(buf + len, tstr_val(replace), replace->len);
            len += replace->len;
            p += search->len;
        } else {
            if (len + 1 > cap) { cap = (len + 1) * 2; buf = tm_realloc(buf, cap); }
            buf[len++] = *p++;
        }
    }
    TStr *out = tstr_new(buf ? buf : "", len);
    if (buf) tm_free(buf);
    return TV_STR(out);
}

static int64_t str_find(TStr *hay, TStr *needle, int64_t from, bool ci, bool reverse) {
    int64_t hl = (int64_t)hay->len, nl = (int64_t)needle->len;
    if (nl == 0) return from <= hl ? from : -1;
    if (nl > hl) return -1;
    if (reverse) {
        for (int64_t i = hl - nl; i >= from; i--) {
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
    for (int64_t i = from; i + nl <= hl; i++) {
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

static TVal bi_strpos(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2) RAISE(vm, "ArgumentCountError", "strpos() expects at least 2 arguments");
    TStr *hay = need_str(vm, &args[0], "strpos", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "strpos", "#2 ($needle)");
    int64_t off = n >= 3 ? need_int(vm, &args[2], "strpos", "#3 ($offset)") : 0;
    if (off < 0) off = 0;
    int64_t r = str_find(hay, needle, off, false, false);
    return r < 0 ? TV_FALSE : TV_INT(r);
}

static TVal bi_stripos(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2) RAISE(vm, "ArgumentCountError", "stripos() expects at least 2 arguments");
    TStr *hay = need_str(vm, &args[0], "stripos", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "stripos", "#2 ($needle)");
    int64_t off = n >= 3 ? need_int(vm, &args[2], "stripos", "#3 ($offset)") : 0;
    if (off < 0) off = 0;
    int64_t r = str_find(hay, needle, off, true, false);
    return r < 0 ? TV_FALSE : TV_INT(r);
}

static TVal bi_strrpos(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2) RAISE(vm, "ArgumentCountError", "strrpos() expects at least 2 arguments");
    TStr *hay = need_str(vm, &args[0], "strrpos", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "strrpos", "#2 ($needle)");
    int64_t r = str_find(hay, needle, 0, false, true);
    return r < 0 ? TV_FALSE : TV_INT(r);
}

static TVal bi_trim_impl(VM *vm, TVal *args, int n, bool left, bool right,
                         const char *defmask) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "trim() expects at least 1 argument");
    TStr *s = need_str(vm, &args[0], "trim", "#1 ($string)");
    const char *mask = defmask;
    size_t masklen = strlen(defmask);
    if (n >= 2) {
        TStr *m = need_str(vm, &args[1], "trim", "#2 ($characters)");
        mask = tstr_val(m);
        masklen = m->len;
    }
    size_t b = 0, e = s->len;
    if (left)
        while (b < e && memchr(mask, s->c[b], masklen)) b++;
    if (right)
        while (e > b && memchr(mask, s->c[e - 1], masklen)) e--;
    return TV_STR(tstr_new(s->c + b, e - b));
}

static TVal bi_trim(VM *vm, TVal *args, int n) {
    return bi_trim_impl(vm, args, n, true, true, " \t\n\r\013\000");
}
static TVal bi_ltrim(VM *vm, TVal *args, int n) {
    return bi_trim_impl(vm, args, n, true, false, " \t\n\r\013\000");
}
static TVal bi_rtrim(VM *vm, TVal *args, int n) {
    return bi_trim_impl(vm, args, n, false, true, " \t\n\r\013\000");
}

static TVal bi_strtolower(VM *vm, TVal *args, int n) {
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

static TVal bi_strtoupper(VM *vm, TVal *args, int n) {
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

static TVal bi_str_pad(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2) RAISE(vm, "ArgumentCountError", "str_pad() expects at least 2 arguments");
    TStr *s = need_str(vm, &args[0], "str_pad", "#1 ($string)");
    int64_t len = need_int(vm, &args[1], "str_pad", "#2 ($length)");
    TStr *pad = n >= 3 ? need_str(vm, &args[2], "str_pad", "#3 ($pad_string)")
                       : (TStr *)tstr_intern(" ", 1);
    if ((int64_t)s->len >= len || pad->len == 0) return args[0];
    if (len > 256 * 1024 * 1024)
        RAISE(vm, "ValueError", "str_pad(): result too large");
    TStr *out = tstr_alloc_raw((uint32_t)len, false);
    memcpy(out->c, tstr_val(s), s->len);
    size_t w = 0;
    for (int64_t i = s->len; i < len; i++) out->c[i] = pad->c[w++ % pad->len];
    out->c[len] = 0;
    return TV_STR(out);
}

static TVal bi_implode(VM *vm, TVal *args, int n) {
    if (n < 1 || n > 2) RAISE(vm, "ArgumentCountError", "implode() expects 1 or 2 arguments");
    TStr *glue;
    TVal *arrv;
    if (n == 2) {
        if (args[0].t == T_ARR && args[1].t == T_STR) {
            arrv = &args[0];
            glue = (TStr *)args[1].u.p;
        } else {
            glue = need_str(vm, &args[0], "implode", "#1 ($separator)");
            arrv = &args[1];
        }
    } else {
        glue = (TStr *)tstr_intern("", 0);
        arrv = &args[0];
    }
    TArr *a = need_arr(vm, arrv, "implode", "array");
    size_t cap = 16, len = 0;
    char *buf = tm_malloc(cap);
    bool first = true;
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TStr *part = tval_to_str(a->data[i]);
        if (!first) {
            while (len + glue->len + 1 > cap) { cap *= 2; buf = tm_realloc(buf, cap); }
            memcpy(buf + len, tstr_val(glue), glue->len);
            len += glue->len;
        }
        first = false;
        while (len + part->len + 1 > cap) { cap *= 2; buf = tm_realloc(buf, cap); }
        memcpy(buf + len, tstr_val(part), part->len);
        len += part->len;
        tval_release(&(TVal){ .u.p = part, .t = T_STR });
    }
    TStr *out = tstr_new(buf, len);
    tm_free(buf);
    return TV_STR(out);
}

static TVal bi_explode(VM *vm, TVal *args, int n) {
    if (n < 2) RAISE(vm, "ArgumentCountError", "explode() expects at least 2 arguments");
    TStr *delim = need_str(vm, &args[0], "explode", "#1 ($separator)");
    TStr *s = need_str(vm, &args[1], "explode", "#2 ($string)");
    int64_t limit = n >= 3 ? need_int(vm, &args[2], "explode", "#3 ($limit)") : INT64_MAX;
    if (delim->len == 0)
        RAISE(vm, "ValueError", "explode(): Argument #1 ($separator) cannot be empty");
    TArr *a = tarr_new();
    const char *p = tstr_val(s);
    const char *end = p + s->len;
    const char *d = tstr_val(delim);
    int64_t count = 0;
    while (p <= end) {
        if (limit >= 0 && count + 1 >= limit) break;
        const char *hit = NULL;
        if ((size_t)(end - p) >= delim->len) {
            for (const char *q = p; q + delim->len <= end; q++)
                if (memcmp(q, d, delim->len) == 0) { hit = q; break; }
        }
        if (!hit) break;
        tarr_append(a, TV_STR(tstr_new(p, (size_t)(hit - p))));
        count++;
        p = hit + delim->len;
        if (p > end) break;
    }
    tarr_append(a, TV_STR(tstr_new(p, (size_t)(end - p))));
    return TV_ARR(a);
}

static TVal bi_str_split(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "str_split() expects at least 1 argument");
    TStr *s = need_str(vm, &args[0], "str_split", "#1 ($string)");
    int64_t len = n >= 2 ? need_int(vm, &args[1], "str_split", "#2 ($length)") : 1;
    if (len < 1)
        RAISE(vm, "ValueError", "str_split(): Argument #2 ($length) must be greater than 0");
    TArr *a = tarr_new();
    for (size_t i = 0; i < s->len; i += (size_t)len) {
        size_t take = s->len - i < (size_t)len ? s->len - i : (size_t)len;
        tarr_append(a, TV_STR(tstr_new(s->c + i, take)));
    }
    return TV_ARR(a);
}

static TVal bi_strrev(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TStr *s = need_str(vm, &args[0], "strrev", "#1 ($string)");
    TStr *out = tstr_alloc_raw(s->len, false);
    for (uint32_t i = 0; i < s->len; i++) out->c[i] = s->c[s->len - 1 - i];
    out->c[s->len] = 0;
    return TV_STR(out);
}

static TVal bi_number_format(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "number_format() expects at least 1 argument");
    double d = tval_to_float(args[0]);
    int64_t dec = n >= 2 ? need_int(vm, &args[1], "number_format", "#2 ($decimals)") : 0;
    const char *dp = n >= 3 ? tstr_val(need_str(vm, &args[2], "number_format", "#3 ($decimal_separator)")) : ".";
    const char *ts = n >= 4 ? tstr_val(need_str(vm, &args[3], "number_format", "#4 ($thousands_separator)")) : ",";
    char tmp[64];
    snprintf(tmp, sizeof tmp, "%.*f", (int)dec, d);
    char out[128];
    size_t oi = 0;
    const char *dot = strchr(tmp, '.');
    size_t ilen = dot ? (size_t)(dot - tmp) : strlen(tmp);
    for (size_t i = 0; i < ilen; i++) {
        out[oi++] = tmp[i];
        size_t remain = ilen - i - 1;
        if (remain > 0 && remain % 3 == 0 && ts[0]) out[oi++] = ts[0];
    }
    if (dot) {
        out[oi++] = dp[0];
        strcpy(out + oi, dot + 1);
        oi += strlen(dot + 1);
    } else {
        out[oi] = 0;
    }
    return TV_STR(tstr_new(out, oi));
}

/* sprintf: common conversions %[flags][width][.precision]sdifxeXobc% */
static TVal bi_sprintf(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "sprintf() expects at least 1 argument");
    TStr *fmt = need_str(vm, &args[0], "sprintf", "#1 ($format)");
    char *out = NULL; size_t cap = 0, len = 0;
#define EMIT(s, sl) do { \
        while (len + (sl) + 1 > cap) { cap = (len + (sl) + 1) * 2; out = tm_realloc(out, cap); } \
        memcpy(out + len, s, sl); len += (sl); \
    } while (0)
    int ai = 1;
    for (size_t i = 0; i < fmt->len; i++) {
        char c = fmt->c[i];
        if (c != '%') { EMIT(&c, 1); continue; }
        i++;
        if (i >= fmt->len) break;
        if (fmt->c[i] == '%') { EMIT("%", 1); continue; }
        bool minus = false, zero = false;
        while (i < fmt->len && (fmt->c[i] == '-' || fmt->c[i] == '0')) {
            if (fmt->c[i] == '-') minus = true; else zero = true;
            i++;
        }
        char wbuf[16]; size_t wi = 0;
        while (i < fmt->len && fmt->c[i] >= '0' && fmt->c[i] <= '9' && wi < 15)
            wbuf[wi++] = fmt->c[i++];
        wbuf[wi] = 0;
        int width = wi ? atoi(wbuf) : 0;
        int prec = -1;
        if (i < fmt->len && fmt->c[i] == '.') {
            i++;
            size_t pi = 0;
            while (i < fmt->len && fmt->c[i] >= '0' && fmt->c[i] <= '9' && pi < 15)
                wbuf[pi++] = fmt->c[i++];
            wbuf[pi] = 0;
            prec = pi ? atoi(wbuf) : 0;
        }
        if (i >= fmt->len) break;
        char conv = fmt->c[i];
        if (ai >= n) RAISE(vm, "ArgumentCountError", "sprintf(): Not enough arguments");
        TVal v = args[ai++];
        char piece[512];
        size_t plen = 0;
        switch (conv) {
        case 'd': case 'u': {
            int64_t iv = tval_to_int(v);
            plen = (size_t)snprintf(piece, sizeof piece, "%lld", (long long)iv);
            break;
        }
        case 'f': {
            double dv = tval_to_float(v);
            plen = (size_t)snprintf(piece, sizeof piece, "%.*f", prec < 0 ? 6 : prec, dv);
            break;
        }
        case 'e': {
            double dv = tval_to_float(v);
            plen = (size_t)snprintf(piece, sizeof piece, "%.*e", prec < 0 ? 6 : prec, dv);
            break;
        }
        case 's': {
            TStr *ss = tval_to_str(v);
            plen = ss->len;
            if (plen >= sizeof piece) plen = sizeof piece - 1;
            memcpy(piece, tstr_val(ss), plen);
            piece[plen] = 0;
            tval_release(&(TVal){ .u.p = ss, .t = T_STR });
            break;
        }
        case 'x': case 'X': {
            int64_t iv = tval_to_int(v);
            plen = (size_t)snprintf(piece, sizeof piece,
                                    conv == 'x' ? "%llx" : "%llX", (unsigned long long)iv);
            break;
        }
        case 'o': {
            int64_t iv = tval_to_int(v);
            plen = (size_t)snprintf(piece, sizeof piece, "%llo", (unsigned long long)iv);
            break;
        }
        case 'b': {
            int64_t iv = tval_to_int(v);
            char bb[70]; int bi2 = 0;
            unsigned long long uv = (unsigned long long)iv;
            if (uv == 0) bb[bi2++] = '0';
            while (uv) { bb[bi2++] = (char)('0' + (uv & 1)); uv >>= 1; }
            for (int k = bi2 - 1; k >= 0; k--) piece[plen++] = bb[k];
            piece[plen] = 0;
            break;
        }
        case 'c': {
            piece[0] = (char)tval_to_int(v);
            piece[1] = 0;
            plen = 1;
            break;
        }
        default:
            RAISE(vm, "ValueError", "sprintf(): Unsupported format specifier '%%%c'", conv);
        }
        if ((int)plen < width) {
            int padn = width - (int)plen;
            char padc = minus ? ' ' : (zero && conv != 's' ? '0' : ' ');
            if (minus) {
                EMIT(piece, plen);
                for (int k = 0; k < padn; k++) EMIT(" ", 1);
            } else {
                for (int k = 0; k < padn; k++) EMIT(&padc, 1);
                EMIT(piece, plen);
            }
        } else {
            EMIT(piece, plen);
        }
    }
    TStr *res = tstr_new(out ? out : "", len);
    if (out) tm_free(out);
    return TV_STR(res);
#undef EMIT
}

static TVal bi_str_contains(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TStr *hay = need_str(vm, &args[0], "str_contains", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "str_contains", "#2 ($needle)");
    return str_find(hay, needle, 0, false, false) >= 0 ? TV_TRUE : TV_FALSE;
}

static TVal bi_str_starts_with(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TStr *hay = need_str(vm, &args[0], "str_starts_with", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "str_starts_with", "#2 ($needle)");
    return hay->len >= needle->len &&
           memcmp(tstr_val(hay), tstr_val(needle), needle->len) == 0
           ? TV_TRUE : TV_FALSE;
}

static TVal bi_str_ends_with(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TStr *hay = need_str(vm, &args[0], "str_ends_with", "#1 ($haystack)");
    TStr *needle = need_str(vm, &args[1], "str_ends_with", "#2 ($needle)");
    return hay->len >= needle->len &&
           memcmp(tstr_val(hay) + (hay->len - needle->len),
                  tstr_val(needle), needle->len) == 0
           ? TV_TRUE : TV_FALSE;
}

/* ---- math ---- */

/* microtime(true)：epoch 浮点秒。uv_hrtime 是单调时钟（非 epoch），
 * 以进程首次调用时的 time(NULL) 为基准换算——耗时差值测量不受影响 */
static TVal bi_microtime(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    static double base_epoch = 0;
    static uint64_t base_hr = 0;
    if (base_epoch == 0) {
        base_epoch = (double)time(NULL);
        base_hr = uv_hrtime();
    }
    return TV_FLOAT(base_epoch +
                    (double)(uv_hrtime() - base_hr) / 1e9);
}

static TVal bi_round(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "round() expects at least 1 argument");
    double d = tval_to_float(args[0]);
    int64_t prec = n >= 2 ? need_int(vm, &args[1], "round", "#2 ($precision)") : 0;
    double mul = pow(10.0, (double)prec);
    double scaled = d * mul;
    double r = scaled >= 0 ? floor(scaled + 0.5) : ceil(scaled - 0.5);
    return TV_FLOAT(r / mul);
}

static TVal bi_floor(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "floor() expects at least 1 argument");
    return TV_FLOAT(floor(tval_to_float(args[0])));
}

static TVal bi_ceil(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "ceil() expects at least 1 argument");
    return TV_FLOAT(ceil(tval_to_float(args[0])));
}

static TVal bi_sqrt(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) RAISE(vm, "ArgumentCountError", "sqrt() expects exactly 1 argument");
    double d = tval_to_float(args[0]);
    if (d < 0) return TV_FLOAT(NAN);
    return TV_FLOAT(sqrt(d));
}

static TVal bi_mt_rand(VM *vm, TVal *args, int n) {
    (void)vm;
    static bool seeded = false;
    if (!seeded) { srand((unsigned)time(NULL)); seeded = true; }
    if (n == 0) return TV_INT(rand());
    if (n < 2) RAISE(vm, "ArgumentCountError", "mt_rand() expects 0 or 2 arguments");
    int64_t lo = need_int(vm, &args[0], "mt_rand", "#1 ($min)");
    int64_t hi = need_int(vm, &args[1], "mt_rand", "#2 ($max)");
    if (hi < lo)
        RAISE(vm, "ValueError", "mt_rand(): Argument #2 ($max) must be greater than or equal to argument #1 ($min)");
    int64_t span = hi - lo + 1;
    return TV_INT(lo + rand() % span);
}

/* ---- registry ---- */



const Builtin tphp_builtins[] = {
    { "print", bi_print },
    { "var_dump", bi_var_dump },
    { "serialize", bi_serialize },
    { "unserialize", bi_unserialize },
    { "print_r", bi_print_r },
    { "count", bi_count },
    { "strlen", bi_strlen },
    { "gettype", bi_gettype },
    { "get_class", bi_get_class },
    { "define", bi_define },
    { "defined", bi_defined },
    { "spl_autoload_register", bi_spl_autoload_register },
    { "class_exists", bi_class_exists },
    { "is_int", bi_is_int },
    { "is_integer", bi_is_int },
    { "is_long", bi_is_int },
    { "is_float", bi_is_float },
    { "is_double", bi_is_float },
    { "is_string", bi_is_string },
    { "is_array", bi_is_array },
    { "is_bool", bi_is_bool },
    { "is_null", bi_is_null },
    { "is_numeric", bi_is_numeric },
    { "abs", bi_abs },
    { "max", bi_max },
    { "min", bi_min },
    { "intdiv", bi_intdiv },
    { "pow", bi_pow },
    { "gc_collect_cycles", bi_gc_collect_cycles },
    { "str_repeat", bi_str_repeat },
    { "exit", bi_exit },
    { "die", bi_exit },
    { "intval", bi_intval },
    { "floatval", bi_floatval },
    { "doubleval", bi_floatval },
    { "strval", bi_strval },
    { "boolval", bi_boolval },
    /* arrays */
    { "array_key_exists", bi_array_key_exists },
    { "in_array", bi_in_array },
    { "array_push", bi_array_push },
    { "array_pop", bi_array_pop },
    { "array_shift", bi_array_shift },
    { "array_unshift", bi_array_unshift },
    { "array_keys", bi_array_keys },
    { "array_values", bi_array_values },
    { "array_merge", bi_array_merge },
    { "array_slice", bi_array_slice },
    { "array_reverse", bi_array_reverse },
    { "array_sum", bi_array_sum },
    { "sort", bi_sort },
    { "rsort", bi_rsort },
    /* strings */
    { "substr", bi_substr },
    { "str_replace", bi_str_replace },
    { "strpos", bi_strpos },
    { "stripos", bi_stripos },
    { "strrpos", bi_strrpos },
    { "trim", bi_trim },
    { "ltrim", bi_ltrim },
    { "rtrim", bi_rtrim },
    { "strtolower", bi_strtolower },
    { "strtoupper", bi_strtoupper },
    { "str_pad", bi_str_pad },
    { "implode", bi_implode },
    { "join", bi_implode },
    { "explode", bi_explode },
    { "str_split", bi_str_split },
    { "strrev", bi_strrev },
    { "number_format", bi_number_format },
    { "sprintf", bi_sprintf },
    { "str_contains", bi_str_contains },
    { "str_starts_with", bi_str_starts_with },
    { "str_ends_with", bi_str_ends_with },
    /* math */
    { "round", bi_round },
    { "microtime", bi_microtime },
    { "floor", bi_floor },
    { "ceil", bi_ceil },
    { "sqrt", bi_sqrt },
    { "mt_rand", bi_mt_rand },
    { "rand", bi_mt_rand },
    /* M4: 协程与时间（sched.c；sleep/usleep 透明挂起，不占 CPU） */
    { "sleep", tphp_async_sleep },
    { "usleep", tphp_async_usleep },
    { "Async\\spawn", tphp_async_spawn },
    { "Async\\await", tphp_async_await },
    { "Async\\suspend", tphp_async_suspend },
    { "Async\\current", tphp_async_current },
    { "Async\\all", tphp_async_all },
    { "Async\\any", tphp_async_any },
    { "Async\\run", tphp_async_run },
    /* M4-b: 透明文件 I/O（io.c；uv_fs 线程池，协程内自动挂起） */
    { "file_get_contents", tphp_io_file_get_contents },
    { "file_put_contents", tphp_io_file_put_contents },
    { "fopen", tphp_io_fopen },
    { "fread", tphp_io_fread },
    { "fwrite", tphp_io_fwrite },
    { "fgets", tphp_io_fgets },
    { "fclose", tphp_io_fclose },
    { "feof", tphp_io_feof },
    { "fseek", tphp_io_fseek },
    { "file_exists", tphp_io_file_exists },
    { "filesize", tphp_io_filesize },
    { "unlink", tphp_io_unlink },
    { "rename", tphp_io_rename },
    { "mkdir", tphp_io_mkdir },
    { "rmdir", tphp_io_rmdir },
    { "scandir", tphp_io_scandir },
    /* M4-d: DNS + TCP（net.c；真异步） */
    { "gethostbyname", tphp_net_gethostbyname },
    { "gethostbynamel", tphp_net_gethostbynamel },
    { "fsockopen", tphp_net_fsockopen },
    { "stream_socket_server", tphp_net_server },
    { "stream_socket_accept", tphp_net_accept },
    { "http_get", tphp_net_http_get },
};

const int tphp_builtin_count = (int)(sizeof tphp_builtins / sizeof tphp_builtins[0]);
