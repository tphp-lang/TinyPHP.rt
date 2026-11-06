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
#include "tsched.h"
#include "tio.h"
#include "pkg.h"
#include "builtin.h"     /* need_str/need_int/str_find 导出（ext/standard 复用） */
#include "standard.h"    /* 迁至 ext/standard 的函数表引用 */
#include "pcre.h"        /* preg_*（ext/pcre） */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

TArr *tarr_new(void);

/* json.c */
TVal bi_json_encode(VM *vm, TVal *args, int n);
TVal bi_json_decode(VM *vm, TVal *args, int n);
TVal bi_json_last_error(VM *vm, TVal *args, int n);
TVal bi_json_last_error_msg(VM *vm, TVal *args, int n);

#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)

/* ---- output core (echo/print) ---- */
void tphp_echo_val(TVal v) {
    TStr *s = tval_to_str(v);
    fwrite(tstr_val(s), 1, tstr_len(s), stdout);
    fflush(stdout);   /* crash-safety: flush so partial output is visible */
    tval_release(&(TVal){ .u.p = s, .t = T_STR });
}

/* ---- var_dump ---- */
static void dump_pad(int n) {
    for (int i = 0; i < n; i++) putchar(' ');
}

static TVal arr_key_at(TArr *a, uint32_t di) {
    if (a->packed) return di < a->n ? TV_INT((int64_t)di) : TV_NULL;
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t != T_NULL && bk->val_off == di)
            return bk->key;
    }
    return TV_NULL;
}

/* 刀13：调用方缓存版键反查——非 packed 源建一次 inv[data_off]=bucket_idx
 * （O(bcap)），此后每键 O(1)；packed 源返回 NULL（arr_key_at_inv 走
 * TV_INT(di) 快路）。修复 unique/flip/chunk/keys 的每元素 O(bcap) 桶反查
 * （审计实测：unique 590×/flip 257×/chunk 684×/keys assoc 50ms）。 */
static uint32_t *arr_key_inv_build(TArr *a) {
    if (a->packed) return NULL;
    uint32_t *inv = tm_malloc(sizeof(uint32_t) *
                              (size_t)(a->dused ? a->dused : 1));
    for (uint32_t i = 0; i < a->dused; i++) inv[i] = UINT32_MAX;
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t != T_NULL && bk->val_off != UINT32_MAX &&
            bk->val_off < a->dused)
            inv[bk->val_off] = b;
    }
    return inv;
}

static TVal arr_key_at_inv(TArr *a, uint32_t di, const uint32_t *inv) {
    if (a->packed) return di < a->n ? TV_INT((int64_t)di) : TV_NULL;
    if (inv && di < a->dused && inv[di] != UINT32_MAX)
        return a->buckets[inv[di]].key;
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
        if (k.t == T_INT) {
            printf("[%lld]=>\n", (long long)k.u.i);
        } else if (k.t == T_STR) {
            TStr *ks = (TStr *)k.u.p;
            printf("[\"%s\"]=>\n", tstr_val(ks));
        } else {
            /* arr_key_at missed (inconsistent or racing hole): never NULL-deref */
            printf("[?%d]=>\n", (int)i);
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
    fflush(stdout);   /* crash-safety: flush partial dump */
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
    case T_OBJ: return "object";
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

/* ---- package manager (pkg.c; CLI 同一核心，测试/脚本化缝) ---- */
static TVal bi_pkg_new(VM *vm, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR || args[1].t != T_STR)
        RAISE(vm, "ArgumentCountError",
              "pkg_new() expects (string $dir, string $name, bool $lib = false)");
    if (n >= 3 && args[2].t != T_TRUE && args[2].t != T_FALSE)
        RAISE(vm, "TypeError", "pkg_new(): Argument #3 ($lib) must be bool");
    const char *dir = tstr_val((TStr *)args[0].u.p);
    const char *name = tstr_val((TStr *)args[1].u.p);
    bool lib = n >= 3 && args[2].t == T_TRUE;
    char err[512] = "";
    int rc = tphp_pkg_new(dir, name, lib, err, sizeof err);
    if (rc == 1)
        RAISE(vm, "ValueError", "pkg_new(): %s", err);
    if (rc != 0)
        RAISE(vm, "RuntimeException", "pkg_new(): %s", err);
    return TV_TRUE;
}

/* pkg_add(string $proj, string $source, string $ref = null) */
static TVal bi_pkg_add(VM *vm, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR || args[1].t != T_STR)
        RAISE(vm, "ArgumentCountError",
              "pkg_add() expects (string $proj, string $source, string $ref = \"\")");
    if (n >= 3 && args[2].t != T_STR && args[2].t != T_NULL)
        RAISE(vm, "TypeError", "pkg_add(): Argument #3 ($ref) must be string or null");
    const char *proj = tstr_val((TStr *)args[0].u.p);
    const char *source = tstr_val((TStr *)args[1].u.p);
    const char *ref = (n >= 3 && args[2].t == T_STR) ? tstr_val((TStr *)args[2].u.p) : NULL;
    char err[512] = "";
    int rc = tphp_pkg_add(proj, source, ref, err, sizeof err);
    if (rc == 1)
        RAISE(vm, "ValueError", "pkg_add(): %s", err);
    if (rc != 0)
        RAISE(vm, "RuntimeException", "pkg_add(): %s", err);
    return TV_TRUE;
}

/* pkg_remove(string $proj, string $name) */
static TVal bi_pkg_remove(VM *vm, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR || args[1].t != T_STR)
        RAISE(vm, "ArgumentCountError",
              "pkg_remove() expects (string $proj, string $name)");
    const char *proj = tstr_val((TStr *)args[0].u.p);
    const char *name = tstr_val((TStr *)args[1].u.p);
    char err[512] = "";
    int rc = tphp_pkg_remove(proj, name, err, sizeof err);
    if (rc == 1)
        RAISE(vm, "ValueError", "pkg_remove(): %s", err);
    if (rc != 0)
        RAISE(vm, "RuntimeException", "pkg_remove(): %s", err);
    return TV_TRUE;
}

/* pkg_install(string $proj, bool $force = false) */
static TVal bi_pkg_install(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "ArgumentCountError",
              "pkg_install() expects (string $proj, bool $force = false)");
    if (n >= 2 && args[1].t != T_TRUE && args[1].t != T_FALSE &&
        args[1].t != T_NULL)
        RAISE(vm, "TypeError", "pkg_install(): Argument #2 ($force) must be bool or null");
    const char *proj = tstr_val((TStr *)args[0].u.p);
    bool force = (n >= 2 && args[1].t == T_TRUE);
    char err[512] = "";
    int rc = tphp_pkg_install(proj, force, err, sizeof err);
    if (rc == 1)
        RAISE(vm, "ValueError", "pkg_install(): %s", err);
    if (rc != 0)
        RAISE(vm, "RuntimeException", "pkg_install(): %s", err);
    return TV_TRUE;
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
    extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
    return class_resolve_lc2(lc, s) ? TV_TRUE : TV_FALSE;
}

/* P3: is_a / is_subclass_of / get_parent_class — object-or-class-string
 * resolution (allow_string defaults to true, matching PHP); a non-object
 * non-string argument yields false (PHP 8 would TypeError, see TODO) */
static TClass *obj_or_class(TVal *v, bool allow_string) {
    if (v->t == T_OBJ) return ((TObj *)v->u.p)->cls;
    if (v->t == T_STR && allow_string) {
        char lc[256];
        const char *s = tstr_val((TStr *)v->u.p);
        size_t k = 0;
        for (; s[k] && k < sizeof lc - 1; k++)
            lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
        lc[k] = 0;
        extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
        return class_resolve_lc2(lc, s);
    }
    return NULL;
}

static bool str_arg_lc(TVal *v, char *lc, size_t cap) {
    if (v->t != T_STR) return false;
    const char *s = tstr_val((TStr *)v->u.p);
    size_t k = 0;
    for (; s[k] && k < cap - 1; k++)
        lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
    lc[k] = 0;
    return true;
}

static TVal bi_is_a(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2)
        RAISE(vm, "ArgumentCountError", "is_a() expects at least 2 arguments");
    if (args[1].t != T_STR)
        RAISE(vm, "TypeError", "is_a(): Argument #2 ($class) must be a string");
    bool allow = (n < 3) ? true : tval_to_bool(args[2]);
    TClass *c = obj_or_class(&args[0], allow);
    char lc[256];
    if (!c || !str_arg_lc(&args[1], lc, sizeof lc)) return TV_FALSE;
    return tphp_class_isa_name(c, lc) ? TV_TRUE : TV_FALSE;
}

static TVal bi_is_subclass_of(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2)
        RAISE(vm, "ArgumentCountError", "is_subclass_of() expects at least 2 arguments");
    if (args[1].t != T_STR)
        RAISE(vm, "TypeError", "is_subclass_of(): Argument #2 ($class) must be a string");
    bool allow = (n < 3) ? true : tval_to_bool(args[2]);
    TClass *c = obj_or_class(&args[0], allow);
    char lc[256];
    if (!c || !str_arg_lc(&args[1], lc, sizeof lc)) return TV_FALSE;
    if (!tphp_class_isa_name(c, lc)) return TV_FALSE;
    extern TClass *class_resolve_lc(const char *fqcn_lc);
    TClass *b = class_resolve_lc(lc);
    /* is_subclass_of excludes the class itself */
    return (b && b == c) ? TV_FALSE : TV_TRUE;
}

static TVal bi_get_parent_class(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 1) return TV_FALSE;
    bool allow = (n < 2) ? true : tval_to_bool(args[1]);
    TClass *c = obj_or_class(&args[0], allow);
    if (!c) return TV_FALSE;
    tphp_class_ensure_flat(c);
    return c->parent ? TV_STR(c->parent->name) : TV_FALSE;
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

TStr *need_str(VM *vm, TVal *v, const char *fn, const char *arg) {
    if (v->t != T_STR)
        RAISE(vm, "TypeError", "%s(): Argument %s must be of type string, %s given",
              fn, arg, type_name(v->t));
    return (TStr *)v->u.p;
}

int64_t need_int(VM *vm, TVal *v, const char *fn, const char *arg) {
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
    tarr_sync_buckets(a);
    /* 原地前插：新值 + 原元素（字符串键保留、数字键重排，对齐 PHP）。
     * 旧实现重建数组写回 args[0]——实参副本，调用方收不到（2026-10-15 修） */
    TVal *vals = tm_malloc(sizeof(TVal) * (size_t)(n - 1 + a->n));
    bool *is_str = tm_malloc(sizeof(bool) * (size_t)(n - 1 + a->n));
    TStr **keys = tm_malloc(sizeof(TStr *) * (size_t)(n - 1 + a->n));
    uint32_t k = 0;
    for (int i = 1; i < n; i++) {
        vals[k] = args[i];
        tval_addref(vals[k]);
        is_str[k] = false;
        keys[k] = NULL;
        k++;
    }
    for (uint32_t i = 0; i < a->dused && k < (uint32_t)(n - 1) + a->n; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal key = arr_key_at(a, i);
        vals[k] = a->data[i];
        tval_addref(vals[k]);
        if (key.t == T_STR) { is_str[k] = true; keys[k] = (TStr *)key.u.p; tval_addref(key); }
        else { is_str[k] = false; keys[k] = NULL; }
        k++;
    }
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_UNDEF)
            tval_release(&a->data[i]);
        a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX && a->buckets[b].key.t == T_STR)
            tval_release(&a->buckets[b].key);
        a->buckets[b].key = TV_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    a->n = 0;
    a->next_idx = 0;
    a->free_head = UINT32_MAX;
    a->dused = 0;
    for (uint32_t i = 0; i < k; i++) {
        if (is_str[i]) {
            TVal key = TV_STR(keys[i]);
            tarr_set(a, key, vals[i]);   /* 所有权转移 */
            tval_release(&key);
        } else {
            tarr_append(a, vals[i]);     /* 所有权转移 */
        }
    }
    tm_free(vals);
    tm_free(is_str);
    tm_free(keys);
    return TV_INT((int64_t)a->n);
}

static TVal bi_array_keys(VM *vm, TVal *args, int n) {
    (void)vm; (void)n;
    TArr *a = need_arr(vm, &args[0], "array_keys", "#1 ($array)");
    uint32_t *inv = arr_key_inv_build(a);   /* 刀13：O(1) 键反查 */
    TArr *b = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal k = arr_key_at_inv(a, i, inv);
        /* 刀13 顺手修既有引用 bug：arr_key_at 返回桶借用，append 接管
         * 引用即亏空（非 packed 键 T_STR 源先释放 = UAF；packed 的
         * TV_INT 无引用故未暴露）——addref 平衡 */
        tval_addref(k);
        tarr_append(b, k);
    }
    if (inv) tm_free(inv);
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

TVal tphp_vm_call_callable(VM *vm, TVal callable, TVal *args, int n);   /* vm.c */
int tphp_vm_call_guarded(VM *vm, TVal callable, TVal *args, int n, TVal *ret);   /* vm.c（刀10：排序比较器/map/filter/reduce 守卫化） */
void *tphp_vm_resolve_fn_global(VM *vm, TStr *name, bool *is_builtin);   /* vm.c（刀13：function_exists） */

/* ---- asort/arsort/ksort/krsort（P1-11）：键保持排序 ----
 * flags 语义对齐 PHP：REGULAR=tval_compare（PHP 8 数值串感知）、
 * NUMERIC=转数值、STRING=字节序（FLAG_CASE=不区分大小写）；
 * LOCALE_STRING 无 locale 支持 → 归 STRING（PLAN/01 §10 差异）。
 * 排序核 = 自顶向下归并（稳定 O(n log n)，对齐 PHP 8.0+ 稳定语义；
 * 既有 sort/rsort/usort 的插入排序 O(n²) 记 backlog 另批升级）。 */
#define PHP_SORT_REGULAR       0
#define PHP_SORT_NUMERIC       1
#define PHP_SORT_STRING        2
#define PHP_SORT_LOCALE_STRING 5
#define PHP_SORT_FLAG_CASE     8

typedef struct { TVal key; TVal val; } SortPair;

/* 排序上下文：flags 路径（asort 族/sort 族）或回调路径（usort） */
typedef struct {
    int flags;
    bool desc, by_key;
    VM *vm;
    TVal cb;                /* usort 回调（args 存活期内借用） */
    bool use_cb;
    bool escaped;           /* 刀10：回调逃逸（异常须上抛）——merge 层快速
                             * 回卷，bi_usort 顶层收尾 kv/tmp 后重抛（防
                             * LONGJMP 穿递归 C 栈泄漏堆分配） */
} SortCtx;

static int sort_flags_cmp(TVal x, TVal y, int flags) {
    bool ci = (flags & PHP_SORT_FLAG_CASE) != 0;
    switch (flags & ~PHP_SORT_FLAG_CASE) {
    case PHP_SORT_NUMERIC: {
        double dx = tval_to_float(x), dy = tval_to_float(y);
        return dx < dy ? -1 : dx > dy ? 1 : 0;
    }
    case PHP_SORT_STRING:
    case PHP_SORT_LOCALE_STRING: {
        TStr *sx = tval_to_str(x), *sy = tval_to_str(y);
        const char *ax = tstr_val(sx), *ay = tstr_val(sy);
        size_t lx = sx->len, ly = sy->len, m = lx < ly ? lx : ly;
        int r = 0;
        if (ci) {
            for (size_t i = 0; i < m; i++) {
                char cx = ax[i], cy = ay[i];
                if (cx >= 'A' && cx <= 'Z') cx += 32;
                if (cy >= 'A' && cy <= 'Z') cy += 32;
                if (cx != cy) { r = (unsigned char)cx < (unsigned char)cy ? -1 : 1; break; }
            }
        } else {
            for (size_t i = 0; i < m; i++)
                if (ax[i] != ay[i]) { r = (unsigned char)ax[i] < (unsigned char)ay[i] ? -1 : 1; break; }
        }
        if (r == 0) r = lx < ly ? -1 : lx > ly ? 1 : 0;
        TVal rx = TV_STR(sx), ry = TV_STR(sy);
        tval_release(&rx);
        tval_release(&ry);
        return r;
    }
    default:
        return tval_compare(x, y);
    }
}

static int sort_pair_cmp(SortCtx *c, TVal x, TVal y) {
    if (c->use_cb) {
        TVal pair[2] = { x, y };
        /* 刀10：guarded 化（镜像 bi_call_user_func）——回调内部 try/catch
         * 捕获自身 raise 时由守卫 pad 正常恢复、排序继续（修复前：longjmp
         * 穿比较器/归并递归 C 栈 → 主分发器内部分恢复 → usort C 帧废弃 →
         * 挂死）；逃逸（异常须上抛）置 escaped 快速回卷，不在此 LONGJMP
         * （kv/tmp 堆分配在 bi_usort 栈帧，穿越即泄漏）。 */
        TVal r;
        if (!tphp_vm_call_guarded(c->vm, c->cb, pair, 2, &r)) {
            c->escaped = true;
            return 0;
        }
        int64_t v = tval_to_int(r);
        tval_release(&r);
        return v > 0 ? 1 : v < 0 ? -1 : 0;
    }
    return sort_flags_cmp(x, y, c->flags);
}

static TVal sort_pair_elem(SortPair *p, SortCtx *c) {
    return c->by_key ? p->key : p->val;
}

static void sort_pairs_merge(SortPair *a, SortPair *tmp, uint32_t lo, uint32_t hi,
                             SortCtx *c) {
    if (hi - lo < 2 || c->escaped) return;      /* 刀10：逃逸快速回卷 */
    uint32_t mid = lo + (hi - lo) / 2;
    sort_pairs_merge(a, tmp, lo, mid, c);
    sort_pairs_merge(a, tmp, mid, hi, c);
    uint32_t i = lo, j = mid, t = lo;
    while (i < mid && j < hi && !c->escaped) {  /* 刀10：逃逸即停比较 */
        int r = sort_pair_cmp(c, sort_pair_elem(&a[i], c), sort_pair_elem(&a[j], c));
        /* 稳定：相等取左（PHP 8.0+ 排序稳定） */
        tmp[t++] = (c->desc ? r >= 0 : r <= 0) ? a[i++] : a[j++];
    }
    while (i < mid) tmp[t++] = a[i++];
    while (j < hi)  tmp[t++] = a[j++];
    memcpy(a + lo, tmp + lo, sizeof(SortPair) * (size_t)(hi - lo));
}

static TVal bi_keep_sort(VM *vm, TVal *args, int n, bool by_key, bool desc,
                         const char *fn) {
    if (n < 1 || n > 2)
        RAISE(vm, "ArgumentCountError", "%s() expects 1 or 2 arguments", fn);
    TArr *a = need_arr(vm, &args[0], fn, "#1 ($array)");
    int flags = n >= 2 ? (int)tval_to_int(args[1]) : 0;
    tarr_sync_buckets(a);
    uint32_t cnt = a->n;
    if (cnt <= 1) return TV_TRUE;
    SortPair *kv = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    SortPair *tmp = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->dused && k < cnt; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        kv[k].val = a->data[i];
        tval_addref(kv[k].val);
        TVal key = TV_NULL;
        for (uint32_t b = 0; b < a->bcap; b++)
            if (a->buckets[b].key.t != T_NULL && a->buckets[b].val_off == i)
                { key = a->buckets[b].key; break; }
        kv[k].key = key;
        tval_addref(key);
        k++;
    }
    SortCtx c = { .flags = flags, .desc = desc, .by_key = by_key,
                  .vm = vm, .cb = TV_NULL, .use_cb = false, .escaped = false };
    sort_pairs_merge(kv, tmp, 0, k, &c);
    /* 清空原数组（同 bi_sort：data 值 + bucket 字符串键；interned 键
     * release no-op，防御式保留判断） */
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_UNDEF)
            tval_release(&a->data[i]);
        a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX && a->buckets[b].key.t == T_STR)
            tval_release(&a->buckets[b].key);
        a->buckets[b].key = TV_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    a->n = 0;
    a->next_idx = 0;
    a->free_head = UINT32_MAX;
    a->dused = 0;
    /* 按 (原键, 值) 重灌——键保持；tarr_set 对字符串键 intern（收集键
     * 已 interned → 表命中），int 键 addref no-op；val 所有权转移 */
    for (uint32_t i = 0; i < k; i++) {
        tarr_set(a, kv[i].key, kv[i].val);   /* set addref key、接管 val */
        tval_release(&kv[i].key);            /* 抵消收集时 +1（值已在桶） */
    }
    tm_free(kv);
    tm_free(tmp);
    return TV_TRUE;
}

static TVal bi_asort(VM *vm, TVal *args, int n) {
    return bi_keep_sort(vm, args, n, false, false, "asort");
}

static TVal bi_arsort(VM *vm, TVal *args, int n) {
    return bi_keep_sort(vm, args, n, false, true, "arsort");
}

static TVal bi_ksort(VM *vm, TVal *args, int n) {
    return bi_keep_sort(vm, args, n, true, false, "ksort");
}

static TVal bi_krsort(VM *vm, TVal *args, int n) {
    return bi_keep_sort(vm, args, n, true, true, "krsort");
}


static TVal bi_sort(VM *vm, TVal *args, int n) {
    TArr *a = need_arr(vm, &args[0], "sort", "#1 ($array)");
    int flags = n >= 2 ? (int)tval_to_int(args[1]) : 0;
    tarr_sync_buckets(a);
    uint32_t cnt = a->n;
    if (cnt <= 1) return TV_TRUE;
    /* 原地排序（P1-11 批：归并核 O(n log n)，替换插入排序 O(n²)；
     * 旧实现写回 args[0] 无效——args 是实参副本，靠共享 TArr 指针原地改） */
    SortPair *kv = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    SortPair *tmp = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->dused && k < cnt; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        kv[k].val = a->data[i];
        tval_addref(kv[k].val);
        kv[k].key = TV_NULL;            /* sort 重排索引，键不参与 */
        k++;
    }
    SortCtx c = { .flags = flags, .desc = false, .by_key = false,
                  .vm = vm, .cb = TV_NULL, .use_cb = false, .escaped = false };
    sort_pairs_merge(kv, tmp, 0, k, &c);
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_UNDEF)
            tval_release(&a->data[i]);
        a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX && a->buckets[b].key.t == T_STR)
            tval_release(&a->buckets[b].key);
        a->buckets[b].key = TV_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    a->n = 0;
    a->next_idx = 0;
    a->free_head = UINT32_MAX;
    a->dused = 0;
    for (uint32_t i = 0; i < k; i++)
        tarr_append(a, kv[i].val);   /* 所有权转移 */
    tm_free(kv);
    tm_free(tmp);
    return TV_TRUE;
}

static TVal bi_rsort(VM *vm, TVal *args, int n) {
    TArr *a = need_arr(vm, &args[0], "rsort", "#1 ($array)");
    int flags = n >= 2 ? (int)tval_to_int(args[1]) : 0;
    tarr_sync_buckets(a);
    uint32_t cnt = a->n;
    if (cnt <= 1) return TV_TRUE;
    SortPair *kv = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    SortPair *tmp = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->dused && k < cnt; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        kv[k].val = a->data[i];
        tval_addref(kv[k].val);
        kv[k].key = TV_NULL;
        k++;
    }
    SortCtx c = { .flags = flags, .desc = true, .by_key = false,
                  .vm = vm, .cb = TV_NULL, .use_cb = false, .escaped = false };
    sort_pairs_merge(kv, tmp, 0, k, &c);
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_UNDEF)
            tval_release(&a->data[i]);
        a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX && a->buckets[b].key.t == T_STR)
            tval_release(&a->buckets[b].key);
        a->buckets[b].key = TV_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    a->n = 0;
    a->next_idx = 0;
    a->free_head = UINT32_MAX;
    a->dused = 0;
    for (uint32_t i = 0; i < k; i++)
        tarr_append(a, kv[i].val);
    tm_free(kv);
    tm_free(tmp);
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

int64_t str_find(TStr *hay, TStr *needle, int64_t from, bool ci, bool reverse) {
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

/* bi_strpos/bi_stripos/bi_strrpos → ext/standard/standard.c */

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

/* bi_strtolower/bi_strtoupper → ext/standard/standard.c */

static TVal bi_str_pad(VM *vm, TVal *args, int n) {
    (void)vm;
    if (n < 2) RAISE(vm, "ArgumentCountError", "str_pad() expects at least 2 arguments");
    TStr *s = need_str(vm, &args[0], "str_pad", "#1 ($string)");
    int64_t len = need_int(vm, &args[1], "str_pad", "#2 ($length)");
    TStr *pad = n >= 3 ? need_str(vm, &args[2], "str_pad", "#3 ($pad_string)")
                       : (TStr *)tstr_intern(" ", 1);
    if (pad->len == 0)
        RAISE(vm, "ValueError", "str_pad(): Argument #3 ($pad_string) must not be empty");
    if ((int64_t)s->len >= len) return args[0];
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

/* 刀7（2026-11-05）explode memchr 快路开关：默认开，TPHP_EXPLODE_FAST=0 关（A/B） */
static int explode_fast_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_EXPLODE_FAST");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 刀7B（11-06）：explode 元素精确分配开关，默认开，TPHP_EXACT_ALLOC=0 关
 *（同二进制 A/B）。元素短命只读、永不 .=，len/4+16 预留全浪费。 */
static int explode_exact_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_EXACT_ALLOC");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}
static TStr *explode_str(const char *s, size_t len) {
    return explode_exact_enabled() ? tstr_new_exact(s, len) : tstr_new(s, len);
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
    /* 刀7快路：单字节分隔符 memchr（12MB 线性 SIMD 扫 vs 通用路逐字节
     * memcmp(q,d,1) 调用——logstat 灾样 "\n"/" " 全中；实测大 explode
     * 6.1× 是 logstat 3.46× 最大单项根因）。语义与通用路逐位等价：
     * p=hit+1 恒 ≤ end（命中前提 q+dlen≤end，dlen=1），p==end 时
     * memchr 长度 0 返 NULL → break → 末段追加空串（尾分隔符语义）；
     * limit 提前截断同款（截断后 p..end 整段作末元素，含后续分隔符）。 */
    if (explode_fast_enabled() && delim->len == 1) {
        char dc = d[0];
        while (p <= end) {
            if (limit >= 0 && count + 1 >= limit) break;
            const char *hit = p < end ? (const char *)memchr(p, dc, (size_t)(end - p)) : NULL;
            if (!hit) break;
            tarr_append(a, TV_STR(explode_str(p, (size_t)(hit - p))));
            count++;
            p = hit + 1;
        }
        tarr_append(a, TV_STR(explode_str(p, (size_t)(end - p))));
        return TV_ARR(a);
    }
    while (p <= end) {
        if (limit >= 0 && count + 1 >= limit) break;
        const char *hit = NULL;
        if ((size_t)(end - p) >= delim->len) {
            for (const char *q = p; q + delim->len <= end; q++)
                if (memcmp(q, d, delim->len) == 0) { hit = q; break; }
        }
        if (!hit) break;
        tarr_append(a, TV_STR(explode_str(p, (size_t)(hit - p))));
        count++;
        p = hit + delim->len;
        if (p > end) break;
    }
    tarr_append(a, TV_STR(explode_str(p, (size_t)(end - p))));
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

/* bi_strrev → ext/standard/standard.c */

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
        if (dp[0]) out[oi++] = dp[0];   /* 空分隔符：跳过（PHP 对拍） */
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
    (void)vm; (void)args; (void)n;
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

/* ---- M6-b: callable 数组函数 + 常用补齐 ---- */

TVal tphp_vm_call_callable(VM *vm, TVal callable, TVal *args, int n);
int tphp_vm_call_guarded(VM *vm, TVal callable, TVal *args, int n, TVal *ret);   /* vm.c */

static void need_callable(VM *vm, TVal *v, const char *fn, const char *arg) {
    /* 统一判定：closure / 函数名 / [obj|'Cls','m'] / 带 __invoke 的对象 */
    if (tphp_vm_is_callable(*v)) return;
    RAISE(vm, "TypeError", "%s(): Argument %s must be callable, %s given",
          fn, arg, type_name(v->t));
}

/* array_map(callable, array, ...): 各数组按最短对齐（PHP8 取最短） */
static TVal bi_array_map(VM *vm, TVal *args, int n) {
    if (n < 2)
        RAISE(vm, "ArgumentCountError", "array_map() expects at least 2 arguments");
    need_callable(vm, &args[0], "array_map", "#1 ($callback)");
    TArr **srcs = tm_malloc(sizeof(TArr *) * (size_t)(n - 1));
    /* 刀12：PHP 8 实测语义 = 多数组**短源 null 填充至最长**（非 min 截断）；
     * 单数组**键保持**（assoc/holey/混合键原样保留，见 backlog P1-16）。 */
    uint32_t max_len = 0;
    for (int i = 1; i < n; i++) {
        if (args[i].t != T_ARR) {
            tm_free(srcs);
            RAISE(vm, "TypeError", "array_map(): Argument #%d must be array", i + 1);
        }
        TArr *a = (TArr *)args[i].u.p;
        srcs[i - 1] = a;
        if (a->n > max_len) max_len = a->n;
    }
    bool single = (n == 2);          /* 单数组：键保持（非 packed 才需 inv） */
    TArr *src0 = NULL;
    uint32_t c0 = 0;                 /* 单数组 data 游标 */
    uint32_t *inv = NULL;            /* 单数组：inv[data_off]=bucket_idx */
    if (single) {
        src0 = srcs[0];
        /* packed 快路：data[i] 键恒 = i（value.h 不变量）→ 键保持 = append
         * （刀11 形态，免 sync/inv）；非 packed（assoc/hash）才走 inv 慢路 */
        if (!src0->packed) {
            tarr_sync_buckets(src0);
            inv = tm_malloc(sizeof(uint32_t) *
                            (size_t)(src0->dused ? src0->dused : 1));
            for (uint32_t i = 0; i < src0->dused; i++) inv[i] = UINT32_MAX;
            for (uint32_t b = 0; b < src0->bcap; b++) {
                uint32_t off = src0->buckets[b].val_off;
                if (off != UINT32_MAX && off < src0->dused) inv[off] = b;
            }
        }
    }
    TArr *out = tarr_new();
    /* 刀11：每源游标推进——原实现每 idx 从 0 重扫"第 idx 个非 UNDEF"=
     * O(n²)（50k 元素实测 847ms vs PHP 1.8ms = 470×；D 裸动态调用对照
     * 反超证明调用机制无罪）；游标单调推进 = 总扫描 O(总元素数)。 */
    uint32_t *curs = single ? NULL
                            : tm_malloc(sizeof(uint32_t) * (size_t)(n - 1));
    if (curs) memset(curs, 0, sizeof(uint32_t) * (size_t)(n - 1));
    for (uint32_t idx = 0; idx < max_len; idx++) {
        TVal *els = tm_malloc(sizeof(TVal) * (size_t)(n - 1));
        TVal key = TV_NULL;          /* 单数组：原键（键保持，实时读桶） */
        if (single) {
            while (c0 < src0->dused && src0->data[c0].t == T_UNDEF) c0++;
            if (c0 < src0->dused) {
                els[0] = src0->data[c0];
                /* packed 快路：key 恒 TV_NULL → append（键恒等 0..n-1）；
                 * 非 packed：inv 反查桶、键实时读（回调改动安全） */
                if (inv && inv[c0] != UINT32_MAX)
                    key = src0->buckets[inv[c0]].key;
                c0++;
            } else {
                els[0] = TV_NULL;    /* 不可达（max_len = 自身 n），防御 */
            }
        } else {
            for (int i = 1; i < n; i++) {
                /* 按插入序取第 idx 个非 UNDEF（游标自上轮停点继续，等价）；
                 * 短源耗尽 → TV_NULL（PHP 8 null 填充语义） */
                TArr *src = srcs[i - 1];
                while (curs[i - 1] < src->dused &&
                       src->data[curs[i - 1]].t == T_UNDEF)
                    curs[i - 1]++;
                els[i - 1] = curs[i - 1] < src->dused
                                 ? src->data[curs[i - 1]++] : TV_NULL;
            }
        }
        /* 刀10：guarded 化（同 bi_call_user_func）——回调内部 catch 正常
         * 继续；逃逸先收尾（els/srcs/inv/curs/out）再重抛，防 LONGJMP 泄漏
         * 堆分配 */
        TVal r;
        if (!tphp_vm_call_guarded(vm, args[0], els, n - 1, &r)) {
            tm_free(els);
            if (inv) tm_free(inv);
            if (curs) tm_free(curs);
            tm_free(srcs);
            TVal ov = TV_ARR(out);
            tval_release(&ov);
            TPHP_LONGJMP(vm->x.unwind, 1);
        }
        if (single && key.t != T_NULL) {
            /* 刀12：单数组键保持（PHP 8 实测 assoc/holey/mixed 原键保留） */
            tval_addref(key);
            tval_addref(r);
            tarr_set(out, key, r);
            tval_release(&key);
            tval_release(&r);
        } else {
            tarr_append(out, r);     /* 多数组：重建 list（PHP 同） */
        }
        tm_free(els);
    }
    if (inv) tm_free(inv);
    if (curs) tm_free(curs);
    tm_free(srcs);
    return TV_ARR(out);
}

/* array_filter(array, callable = null): 无回调时过滤 falsy。
 * 刀11：键获取改 inv[] 反查（原实现每元素反查全桶表 = O(n²)——50k 实测
 * 965ms vs PHP 2.9ms = 333×；一次遍历建 inv[val_off]=bucket_idx 反向索引
 * → data 序遍历 + O(1) 键，序与键语义不变；键实时读桶 = 回调改动源数组
 * 安全（原版同级），NULL fallback append 保留）。 */
static TVal bi_array_filter(VM *vm, TVal *args, int n) {
    if (n < 1 || n > 2)
        RAISE(vm, "ArgumentCountError", "array_filter() expects 1 or 2 arguments");
    need_arr(vm, &args[0], "array_filter", "#1 ($array)");
    if (n >= 2) need_callable(vm, &args[1], "array_filter", "#2 ($callback)");
    TArr *a = (TArr *)args[0].u.p;
    /* 刀12：packed 快路——键恒 = i（value.h 不变量）→ 免 sync/inv，
     * 键 TV_INT(i) 直构；非 packed（assoc/hash）才 sync + inv 反查 */
    bool fast = a->packed;
    if (!fast) tarr_sync_buckets(a);
    uint32_t du = a->dused;
    uint32_t *inv = fast ? NULL
                         : tm_malloc(sizeof(uint32_t) * (size_t)(du ? du : 1));
    if (!fast) {
        for (uint32_t i = 0; i < du; i++) inv[i] = UINT32_MAX;
        for (uint32_t b = 0; b < a->bcap; b++) {
            uint32_t off = a->buckets[b].val_off;
            if (off != UINT32_MAX && off < du) inv[off] = b;
        }
    }
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < du; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        bool keep;
        if (n >= 2) {
            TVal arg = a->data[i];
            /* 刀10：guarded 化（同 bi_call_user_func）；逃逸收尾 out 重抛 */
            TVal r;
            if (!tphp_vm_call_guarded(vm, args[1], &arg, 1, &r)) {
                if (inv) tm_free(inv);
                TVal ov = TV_ARR(out);
                tval_release(&ov);
                TPHP_LONGJMP(vm->x.unwind, 1);
            }
            keep = tval_to_bool(r);
            tval_release(&r);
        } else {
            keep = tval_to_bool(a->data[i]);
        }
        if (keep) {
            /* 保留原键（packed：键 = i；非 packed：inv[off] 反查桶，键实时读桶） */
            tval_addref(a->data[i]);
            TVal v = a->data[i];
            TVal key = TV_NULL;
            if (fast) key = TV_INT((int64_t)i);
            else if (inv[i] != UINT32_MAX) key = a->buckets[inv[i]].key;
            if (key.t == T_NULL) tarr_append(out, v);
            else {
                tval_addref(key);
                tarr_set(out, key, v);
                tval_release(&key);
            }
        }
    }
    if (inv) tm_free(inv);
    return TV_ARR(out);
}

/* array_reduce(array, callable, initial = null) */
static TVal bi_array_reduce(VM *vm, TVal *args, int n) {
    if (n < 2 || n > 3)
        RAISE(vm, "ArgumentCountError", "array_reduce() expects 2 or 3 arguments");
    need_arr(vm, &args[0], "array_reduce", "#1 ($array)");
    need_callable(vm, &args[1], "array_reduce", "#2 ($callback)");
    TArr *a = (TArr *)args[0].u.p;
    TVal carry = n >= 3 ? args[2] : TV_NULL;
    tval_addref(carry);
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal fargs[2] = { carry, a->data[i] };
        /* 刀10：guarded 化（同 bi_call_user_func）；逃逸收尾 carry 重抛 */
        TVal r;
        if (!tphp_vm_call_guarded(vm, args[1], fargs, 2, &r)) {
            tval_release(&carry);
            TPHP_LONGJMP(vm->x.unwind, 1);
        }
        tval_release(&carry);
        carry = r;   /* 转移 */
    }
    return carry;
}

/* usort(array, callable): 按键重排（返回 true），比较函数 <0/0/>0 */
static TVal bi_usort(VM *vm, TVal *args, int n) {
    if (n != 2)
        RAISE(vm, "ArgumentCountError", "usort() expects exactly 2 arguments");
    need_arr(vm, &args[0], "usort", "#1 ( &$array )");
    need_callable(vm, &args[1], "usort", "#2 ($callback)");
    TArr *a = (TArr *)args[0].u.p;
    tarr_sync_buckets(a);
    uint32_t cnt = a->n;
    if (cnt <= 1) return TV_TRUE;
    /* 收集元素（P1-11 批：归并核 O(n log n)，替换插入排序 O(n²)） */
    SortPair *kv = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    SortPair *tmp = tm_malloc(sizeof(SortPair) * (size_t)cnt);
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->dused && k < cnt; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        kv[k].val = a->data[i];
        tval_addref(kv[k].val);
        kv[k].key = TV_NULL;
        k++;
    }
    SortCtx c = { .flags = 0, .desc = false, .by_key = false,
                  .vm = vm, .cb = args[1], .use_cb = true, .escaped = false };
    sort_pairs_merge(kv, tmp, 0, k, &c);
    if (c.escaped) {
        /* 刀10：比较器异常逃逸——数组未动（写回在排序后）= PHP"异常时数组
         * 状态未定义"的安全子集；释放 kv 持有的元素引用 + 堆分配后重抛
         * （unwind 目标 = 本 builtin 调用链上方的主 pad，与既有 builtin
         * raise 同路径）。 */
        for (uint32_t i = 0; i < k; i++) {
            tval_release(&kv[i].val);
            if (kv[i].key.t != T_NULL) tval_release(&kv[i].key);
        }
        tm_free(kv);
        tm_free(tmp);
        TPHP_LONGJMP(vm->x.unwind, 1);
    }
    /* 清空原数组后按序写回（int 键 0..n-1） */
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_UNDEF)
            tval_release(&a->data[i]);
        a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX && a->buckets[b].key.t == T_STR)
            tval_release(&a->buckets[b].key);
        a->buckets[b].key = TV_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    a->n = 0;
    a->next_idx = 0;
    a->free_head = UINT32_MAX;
    a->dused = 0;
    /* 重新填充 */
    for (uint32_t i = 0; i < k; i++)
        tarr_append(a, kv[i].val);   /* 所有权转移 */
    tm_free(kv);
    tm_free(tmp);
    return TV_TRUE;
}

/* array_flip(array): 键值互换 */
static TVal bi_array_flip(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "array_flip() expects exactly 1 argument");
    need_arr(vm, &args[0], "array_flip", "#1 ($array)");
    TArr *a = (TArr *)args[0].u.p;
    tarr_sync_buckets(a);
    uint32_t *inv = arr_key_inv_build(a);   /* 刀13：O(1) 键反查（原每元素 O(bcap) 反查 = 50k 180ms vs PHP 0.7ms） */
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal key = arr_key_at_inv(a, i, inv);
        TVal v = a->data[i];
        /* 值必须是 int/string 才能当键 */
        if (v.t != T_INT && v.t != T_STR) continue;
        tval_addref(v);          /* 新键 */
        TVal fk = key.t == T_NULL ? TV_INT((int64_t)i) : key;
        tval_addref(fk);
        tarr_set(out, v, fk);
        tval_release(&v);
        tval_release(&fk);
    }
    if (inv) tm_free(inv);
    return TV_ARR(out);
}

/* 刀13：strict value set（INT/STR）——**不复用 tarr**：tarr 键空间 = PHP
 * 数组键语义（数字串 norm 折算先行，"1"/"01"/1 同键），unique 需要
 * identity set（tag + 值/串内容严格）——norm 折算会误去重（实测漂移：
 * [1, 2, "1"] 刀13 首版丢 "1"）。自建 open addressing，cap 一次到位免
 * 扩容；keys 存借用（源元素引用 ≥1 存活于本调用栈，安全）。 */
typedef struct { TVal *keys; uint8_t *used; uint32_t cap; } VSet;
static uint32_t vset_hash(TVal v) {
    if (v.t == T_INT) {
        uint64_t x = (uint64_t)v.u.i;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
        return (uint32_t)(x | 1);
    }
    return TSTR_HASH((TStr *)v.u.p);
}
static bool vset_eq(TVal a, TVal b) {
    if (a.t != b.t) return false;
    if (a.t == T_INT) return a.u.i == b.u.i;
    return tstr_eq((TStr *)a.u.p, (TStr *)b.u.p);
}
static void vset_init(VSet *s, uint32_t n) {
    s->cap = 64;
    while (s->cap < n * 2 + 8) s->cap <<= 1;
    s->keys = tm_malloc(sizeof(TVal) * s->cap);
    s->used = tm_malloc(s->cap);
    memset(s->used, 0, s->cap);
}
static bool vset_has(const VSet *s, TVal v) {
    uint32_t i = vset_hash(v) & (s->cap - 1);
    while (s->used[i]) {
        if (vset_eq(s->keys[i], v)) return true;
        i = (i + 1) & (s->cap - 1);
    }
    return false;
}
static void vset_add(VSet *s, TVal v) {
    uint32_t i = vset_hash(v) & (s->cap - 1);
    while (s->used[i]) {
        if (vset_eq(s->keys[i], v)) return;
        i = (i + 1) & (s->cap - 1);
    }
    s->used[i] = 1;
    s->keys[i] = v;
}
/* 刀14：持有型释放（diff/intersect 的键 = tval_to_str 新引用，归属 set）。
 * vset_add 本身存值拷贝（借用指针），引用策略归调用方：unique 借用源元素
 * （不调本函数），diff/intersect 的键引用由本函数统一收。 */
static void vset_free_strs(VSet *s) {
    for (uint32_t i = 0; i < s->cap; i++)
        if (s->used[i] && s->keys[i].t == T_STR) tval_release(&s->keys[i]);
    tm_free(s->keys);
    tm_free(s->used);
}

/* 刀14：array_diff / array_intersect 共用核（PHP 默认 (string) 比较语义
 * ——值字符串化后 VSet 严格内容匹配，"1" 与 1 同串判重 ✓ 实测对齐）。
 * keep_mode = true 保留命中（intersect）/ false 保留未命中（diff）；
 * 键保持首数组原键（assoc/holey，PHP 实测）。 */
static TVal k14_diff_intersect(VM *vm, TVal *args, int n, bool keep_mode,
                               const char *fn) {
    TArr *a = need_arr(vm, &args[0], fn, "#1 ($array)");
    VSet ex;
    uint32_t total = 0;
    for (int ai = 1; ai < n; ai++) {
        TArr *s = need_arr(vm, &args[ai], fn, "variable list");
        total += s->dused;
    }
    vset_init(&ex, total ? total : 1);
    for (int ai = 1; ai < n; ai++) {
        TArr *s = (TArr *)args[ai].u.p;
        for (uint32_t j = 0; j < s->dused; j++) {
            TVal v = s->data[j];
            if (v.t == T_UNDEF) continue;
            TStr *sv = tval_to_str(v);       /* 新引用，归属 ex */
            TVal k = TV_STR(sv);
            vset_add(&ex, k);
            /* sv 引用由 vset_free_strs 统一收（vset_add 存借用指针） */
        }
    }
    uint32_t *inv = arr_key_inv_build(a);
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        TVal v = a->data[i];
        if (v.t == T_UNDEF) continue;
        TStr *sv = tval_to_str(v);
        TVal k = TV_STR(sv);
        bool hit = vset_has(&ex, k);
        tval_release(&k);                    /* 比较用临时串，立即收 */
        if (hit != keep_mode) continue;
        TVal key = arr_key_at_inv(a, i, inv);
        tval_addref(v);
        if (key.t == T_NULL) tarr_append(out, v);
        else {
            tval_addref(key);
            tarr_set(out, key, v);
            tval_release(&key);
        }
    }
    vset_free_strs(&ex);
    if (inv) tm_free(inv);
    return TV_ARR(out);
}
static TVal bi_array_diff(VM *vm, TVal *args, int n) {
    if (n < 2)
        RAISE(vm, "ArgumentCountError", "array_diff() expects at least 2 arguments");
    for (int i = 0; i < n; i++)
        need_arr(vm, &args[i], "array_diff", i == 0 ? "#1 ($array)" : "parameter list");
    return k14_diff_intersect(vm, args, n, false, "array_diff");
}
static TVal bi_array_intersect(VM *vm, TVal *args, int n) {
    if (n < 2)
        RAISE(vm, "ArgumentCountError", "array_intersect() expects at least 2 arguments");
    for (int i = 0; i < n; i++)
        need_arr(vm, &args[i], "array_intersect", i == 0 ? "#1 ($array)" : "parameter list");
    return k14_diff_intersect(vm, args, n, true, "array_intersect");
}

/* array_count_values(array): 值计数。tarr 键空间（数字串折算）恰好与 PHP
 * 数组键语义一致（实测 [1,"1"] → {"1":3} 合并计数 ✓）；非 int/str 值
 * PHP 发 Warning 跳过——tphp 无 warning 体系，静默跳过（差异记录）。 */
static TVal bi_array_count_values(VM *vm, TVal *args, int n) {
    if (n != 1)
        RAISE(vm, "ArgumentCountError", "array_count_values() expects exactly 1 argument");
    TArr *a = need_arr(vm, &args[0], "array_count_values", "#1 ($array)");
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        TVal v = a->data[i];
        if (v.t == T_UNDEF) continue;
        if (v.t != T_INT && v.t != T_STR) continue;
        TVal *c = tarr_get(out, v);
        if (c) { c->u.i++; }
        else { tval_addref(v); tarr_set(out, v, TV_INT(1)); }
    }
    return TV_ARR(out);
}

/* array_unique(array): 按值去重，保留首次出现。
 * 刀13：原实现 = 每元素 O(out) 重扫 + O(bcap) 桶反查（双重 O(n²)）——50k
 * 实测 851ms vs PHP 1.4ms = 590×。修复 = INT/STR 走 VSet strict identity
 * set + inv[] 键反查；FLOAT/NULL/BOOL/ARR 元素（set 仅支持 int/str）回退
 * aux 列表线性扫（频次低，通常空）。语义 = 既有 strict_eq 逐值等价
 * （**不改 PHP 默认 SORT_STRING 行为**——那是另一个既有差异，另案）。 */
static TVal bi_array_unique(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "array_unique() expects exactly 1 argument");
    need_arr(vm, &args[0], "array_unique", "#1 ($array)");
    TArr *a = (TArr *)args[0].u.p;
    tarr_sync_buckets(a);
    uint32_t *inv = arr_key_inv_build(a);
    VSet seen;
    vset_init(&seen, a->dused);
    TArr *aux = tarr_new();      /* 其他类型首现值（线性，通常空） */
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        TVal v = a->data[i];
        bool dup;
        if (v.t == T_INT || v.t == T_STR) {
            dup = vset_has(&seen, v);
        } else {
            dup = false;
            for (uint32_t j = 0; j < aux->dused; j++)
                if (tval_strict_eq(v, aux->data[j])) { dup = true; break; }
        }
        if (!dup) {
            if (v.t == T_INT || v.t == T_STR) {
                vset_add(&seen, v);
            } else {
                tval_addref(v);
                tarr_append(aux, v);
            }
            TVal key = arr_key_at_inv(a, i, inv);
            tval_addref(v);
            if (key.t == T_NULL) tarr_append(out, v);
            else {
                tval_addref(key);
                tarr_set(out, key, v);
                tval_release(&key);
            }
        }
    }
    TVal av = TV_ARR(aux);
    tval_release(&av);
    tm_free(seen.keys);
    tm_free(seen.used);
    if (inv) tm_free(inv);
    return TV_ARR(out);
}

/* array_search(needle, haystack[, strict=false]) */
static TVal bi_array_search(VM *vm, TVal *args, int n) {
    if (n < 2 || n > 3)
        RAISE(vm, "ArgumentCountError", "array_search() expects 2 or 3 arguments");
    need_arr(vm, &args[1], "array_search", "#2 ($haystack)");
    bool strict = n >= 3 ? tval_to_bool(args[2]) : false;
    TArr *a = (TArr *)args[1].u.p;
    tarr_sync_buckets(a);
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        bool hit = strict ? tval_strict_eq(args[0], a->data[i])
                          : tval_loose_eq(args[0], a->data[i]);
        if (hit) {
            for (uint32_t b = 0; b < a->bcap; b++)
                if (a->buckets[b].val_off == i && a->buckets[b].key.t != T_NULL) {
                    TVal k = a->buckets[b].key;
                    tval_addref(k);
                    return k;
                }
            return TV_INT((int64_t)i);
        }
    }
    return TV_FALSE;
}

/* range(start, end[, step]) */
static TVal bi_range(VM *vm, TVal *args, int n) {
    if (n < 2 || n > 3)
        RAISE(vm, "ArgumentCountError", "range() expects 2 or 3 arguments");
    if (args[0].t == T_STR && args[1].t == T_STR) {
        /* 字符串范围 a-z */
        if (tstr_len((TStr *)args[0].u.p) != 1 || tstr_len((TStr *)args[1].u.p) != 1)
            RAISE(vm, "TypeError", "range(): strings must be single characters");
        char c0 = tstr_val((TStr *)args[0].u.p)[0];
        char c1 = tstr_val((TStr *)args[1].u.p)[0];
        int step = n >= 3 ? (int)tval_to_int(args[2]) : 1;
        if (step == 0) RAISE(vm, "ValueError", "range(): step cannot be 0");
        TArr *out = tarr_new();
        if (step > 0) {
            for (int c = c0; c <= c1; c += step) {
                char s[2] = { (char)c, 0 };
                tarr_append(out, TV_STR(tstr_new(s, 1)));
            }
        } else {
            for (int c = c0; c >= c1; c += step) {
                char s[2] = { (char)c, 0 };
                tarr_append(out, TV_STR(tstr_new(s, 1)));
            }
        }
        return TV_ARR(out);
    }
    double d0 = tval_to_float(args[0]);
    double d1 = tval_to_float(args[1]);
    double step = n >= 3 ? tval_to_float(args[2]) : 1.0;
    if (step == 0.0) RAISE(vm, "ValueError", "range(): step cannot be 0");
    bool all_int = args[0].t == T_INT && args[1].t == T_INT &&
                   (n < 3 || args[2].t == T_INT);
    TArr *out = tarr_new();
    if (step > 0) {
        for (double x = d0; x <= d1 + 1e-12; x += step) {
            if (all_int) tarr_append(out, TV_INT((int64_t)x));
            else tarr_append(out, TV_FLOAT(x));
        }
    } else {
        for (double x = d0; x >= d1 - 1e-12; x += step) {
            if (all_int) tarr_append(out, TV_INT((int64_t)x));
            else tarr_append(out, TV_FLOAT(x));
        }
    }
    return TV_ARR(out);
}

/* array_key_first / array_key_last */
static TVal bi_array_key_first(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "array_key_first() expects exactly 1 argument");
    need_arr(vm, &args[0], "array_key_first", "#1 ($array)");
    TArr *a = (TArr *)args[0].u.p;
    tarr_sync_buckets(a);
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        for (uint32_t b = 0; b < a->bcap; b++)
            if (a->buckets[b].val_off == i && a->buckets[b].key.t != T_NULL) {
                TVal k = a->buckets[b].key;
                tval_addref(k);
                return k;
            }
    }
    return TV_NULL;
}
static TVal bi_array_key_last(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "array_key_last() expects exactly 1 argument");
    need_arr(vm, &args[0], "array_key_last", "#1 ($array)");
    TArr *a = (TArr *)args[0].u.p;
    tarr_sync_buckets(a);
    for (uint32_t i = a->dused; i > 0; i--) {
        uint32_t j = i - 1;
        if (a->data[j].t == T_UNDEF) continue;
        for (uint32_t b = 0; b < a->bcap; b++)
            if (a->buckets[b].val_off == j && a->buckets[b].key.t != T_NULL) {
                TVal k = a->buckets[b].key;
                tval_addref(k);
                return k;
            }
    }
    return TV_NULL;
}

/* chr / ord */
static TVal bi_chr(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "chr() expects exactly 1 argument");
    int64_t c = need_int(vm, &args[0], "chr", "#1 ($codepoint)");
    if (c < 0 || c > 255)
        RAISE(vm, "ValueError", "chr(): Argument #1 ($codepoint) must be between 0 and 255");
    char s[2] = { (char)c, 0 };
    return TV_STR(tstr_new(s, 1));
}
static TVal bi_ord(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "ord() expects exactly 1 argument");
    TStr *s = need_str(vm, &args[0], "ord", "#1 ($string)");
    if (s->len == 0) return TV_INT(0);
    return TV_INT((unsigned char)s->c[0]);
}

/* call_user_func(callable, ...$args)
 * 刀9（2026-11-06）：改走 tphp_vm_call_guarded——此前直调 call_callable
 * 无守卫 pad，callee 内部 try/catch 捕获自身 raise 时：longjmp 穿过本
 * builtin 的 C 帧 → 主分发器在 callee 帧内部分恢复（catch 跑完、帧弹出）
 * → 本 C 帧已废弃、调用点 dst 永不写 → 主循环踩未写值挂死（JIT=0 纯解
 * 释器同样挂，PHP 正常——2026-11-06 刀9 语义矩阵 case 8 实测）。
 * guarded 两路：正常完成（含 callee 内部 catch）返 1 正常返回；逃逸
 * （异常须上抛）返 0 且 exception 已设 → LONGJMP 重抛（当前 unwind 目标
 * = 本 builtin 调用链上方的主 pad，与既有 builtin raise 同路径）。 */
static TVal bi_call_user_func(VM *vm, TVal *args, int n) {
    if (n < 1) RAISE(vm, "ArgumentCountError", "call_user_func() expects at least 1 argument");
    need_callable(vm, &args[0], "call_user_func", "#1 ($callback)");
    TVal r;
    if (!tphp_vm_call_guarded(vm, args[0], args + 1, n - 1, &r))
        TPHP_LONGJMP(vm->x.unwind, 1);      /* 逃逸：exception 已设，重抛 */
    return r;
}

/* call_user_func_array(callable, array) —— 守卫同 bi_call_user_func（刀9） */
static TVal bi_call_user_func_array(VM *vm, TVal *args, int n) {
    if (n != 2) RAISE(vm, "ArgumentCountError", "call_user_func_array() expects exactly 2 arguments");
    need_callable(vm, &args[0], "call_user_func_array", "#1 ($callback)");
    need_arr(vm, &args[1], "call_user_func_array", "#2 ($args)");
    TArr *a = (TArr *)args[1].u.p;
    TVal *els = tm_malloc(sizeof(TVal) * (a->n ? a->n : 1));
    int k = 0;
    for (uint32_t i = 0; i < a->dused; i++)
        if (a->data[i].t != T_UNDEF) els[k++] = a->data[i];
    TVal r;
    int ok = tphp_vm_call_guarded(vm, args[0], els, k, &r);
    tm_free(els);
    if (!ok) TPHP_LONGJMP(vm->x.unwind, 1); /* 逃逸：exception 已设，重抛 */
    return r;
}

/* strcmp(string $a, string $b): 字节序比较（二进制安全）。P1-15（刀11）：
 * usort 字符串惯用形态 `strlen($a)-strlen($b) ?: strcmp($a,$b)` 直接
 * undefined function。PHP 8 契约 = 返回值归一 -1/0/1（实测 php -r 确认）。 */
static TVal bi_strcmp(VM *vm, TVal *args, int n) {
    if (n != 2)
        RAISE(vm, "ArgumentCountError", "strcmp() expects exactly 2 arguments");
    TStr *a = need_str(vm, &args[0], "strcmp", "#1 ($string1)");
    TStr *b = need_str(vm, &args[1], "strcmp", "#2 ($string2)");
    size_t la = a->len, lb = b->len;
    size_t lm = la < lb ? la : lb;
    int c = lm ? memcmp(a->c, b->c, lm) : 0;
    if (c == 0) c = la < lb ? -1 : la > lb ? 1 : 0;
    return TV_INT(c > 0 ? 1 : c < 0 ? -1 : 0);
}

/* strncmp/strcasecmp/strncasecmp（P1-15 同族，刀13）：字节序比较 +
 * PHP 8 归一契约 -1/0/1；strcasecmp 族 = ASCII tolower（C locale 语义） */
#define K13_LC(c) ((c) >= 'A' && (c) <= 'Z' ? (c) | 0x20 : (c))
/* PHP 8.5 实测契约（php -r 逐例取证）：strcmp/strcasecmp/strncmp 归一
 * -1/0/1；strncasecmp **字节不等返回 tolower 字节差**（strncasecmp('Abc',
 * 'XYZ',2) = -23 未归一）而长度决胜归一 ±1（'ab' vs 'abc' len3 = -1 非
 * -99）——raw = true 仅供 strncasecmp。 */
static int k13_bincmp(const char *a, size_t la, const char *b, size_t lb,
                      size_t maxlen, bool ci, bool raw) {
    size_t lm = la < lb ? la : lb;
    if (maxlen && lm > maxlen) lm = maxlen;
    for (size_t i = 0; i < lm; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (ci) { x = K13_LC(x); y = K13_LC(y); }
        if (x != y) {
            if (raw) return (int)x - (int)y;   /* strncasecmp：字节差 */
            return x < y ? -1 : 1;
        }
    }
    if (maxlen && lm == maxlen && lm < la && lm < lb) return 0;   /* 双截断（PHP 实测 strncmp("abcdef","abcxyz",3)=0） */
    if (lm < la || lm < lb) return la < lb ? -1 : 1;   /* 一串先尽：±1 */
    return 0;
}
static TVal bi_strncmp(VM *vm, TVal *args, int n) {
    if (n != 3)
        RAISE(vm, "ArgumentCountError", "strncmp() expects exactly 3 arguments");
    TStr *a = need_str(vm, &args[0], "strncmp", "#1 ($string1)");
    TStr *b = need_str(vm, &args[1], "strncmp", "#2 ($string2)");
    int64_t len = need_int(vm, &args[2], "strncmp", "#3 ($length)");
    if (len < 0)
        RAISE(vm, "ValueError", "strncmp(): Argument #3 ($length) must be greater than or equal to 0");
    int c = k13_bincmp(a->c, a->len, b->c, b->len, (size_t)len, false, false);
    return TV_INT(c > 0 ? 1 : c < 0 ? -1 : 0);
}
static TVal bi_strcasecmp(VM *vm, TVal *args, int n) {
    if (n != 2)
        RAISE(vm, "ArgumentCountError", "strcasecmp() expects exactly 2 arguments");
    TStr *a = need_str(vm, &args[0], "strcasecmp", "#1 ($string1)");
    TStr *b = need_str(vm, &args[1], "strcasecmp", "#2 ($string2)");
    int c = k13_bincmp(a->c, a->len, b->c, b->len, 0, true, false);
    return TV_INT(c > 0 ? 1 : c < 0 ? -1 : 0);
}
static TVal bi_strncasecmp(VM *vm, TVal *args, int n) {
    if (n != 3)
        RAISE(vm, "ArgumentCountError", "strncasecmp() expects exactly 3 arguments");
    TStr *a = need_str(vm, &args[0], "strncasecmp", "#1 ($string1)");
    TStr *b = need_str(vm, &args[1], "strncasecmp", "#2 ($string2)");
    int64_t len = need_int(vm, &args[2], "strncasecmp", "#3 ($length)");
    if (len < 0)
        RAISE(vm, "ValueError", "strncasecmp(): Argument #3 ($length) must be greater than or equal to 0");
    return TV_INT(k13_bincmp(a->c, a->len, b->c, b->len, (size_t)len, true, true));
}
#undef K13_LC

/* function_exists(string): 内建 + 已加载用户函数（tphp_vm_resolve_fn_global
 * 同源 = is_callable 的函数名分支修正——is_callable 对未定义名误报 true
 * 的 bug 另案记录，本函数走真解析路径）。 */
static TVal bi_function_exists(VM *vm, TVal *args, int n) {
    if (n != 1)
        RAISE(vm, "ArgumentCountError", "function_exists() expects exactly 1 argument");
    TStr *name = need_str(vm, &args[0], "function_exists", "#1 ($function)");
    bool isb = false;
    void *hit = tphp_vm_resolve_fn_global(vm, name, &isb);
    return hit ? TV_TRUE : TV_FALSE;
}

/* is_callable(mixed) */
/* is_callable(mixed, syntax_only = false): 用户面 strict（PHP 默认真解析）。
 * syntax_only=true 走 tphp_vm_is_callable 粗判；默认（false）按形态真查：
 * 函数名 resolve / 'Cls::m' 与 [obj,'m'] 方法存在性 / __invoke——修复
 * 未定义名误报 true（is_callable("no_such_fn_zz") PHP=false）。调用前置
 * need_callable 仍走粗判（错误消息语义 = 调用点 undefined，PHP 对齐）。 */
static bool k14_strict_callable(VM *vm, TVal v) {
    switch (v.t) {
    case T_CLOSURE:
        return true;
    case T_STR: {
        const char *s = tstr_val((TStr *)v.u.p);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2) sep = p;
        if (sep) {
            size_t cl = (size_t)(sep - s);
            char clc[256], mlc[128];
            if (cl >= sizeof clc) cl = sizeof clc - 1;
            for (size_t i = 0; i < cl; i++)
                clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
            clc[cl] = 0;
            { const char *s2 = sep + 2;
              size_t k = 0; for (; s2[k] && k < sizeof mlc - 1; k++)
                  mlc[k] = (s2[k] >= 'A' && s2[k] <= 'Z') ? (char)(s2[k] + 32) : s2[k];
              mlc[k] = 0; }
            extern TClass *class_resolve_lc(const char *fqcn_lc);
            TClass *cls = class_resolve_lc(clc);
            if (!cls) return false;
            tphp_class_ensure_flat(cls);
            TMethod *m = tphp_class_find_method(cls, mlc);
            return m && (m->fn || m->native) && !m->is_abstract;
        }
        bool isb = false;
        return tphp_vm_resolve_fn_global(vm, (TStr *)v.u.p, &isb) != NULL;
    }
    case T_ARR: {
        TArr *a = (TArr *)v.u.p;
        if (a->n != 2) return false;
        TVal e[2];
        int k = 0;
        for (uint32_t i = 0; i < a->dused && k < 2; i++)
            if (a->data[i].t != T_UNDEF) e[k++] = a->data[i];
        if (k != 2 || e[1].t != T_STR) return false;
        const char *mlc_s = tstr_val((TStr *)e[1].u.p);
        char mlc[128];
        { size_t k2 = 0; for (; mlc_s[k2] && k2 < sizeof mlc - 1; k2++)
            mlc[k2] = (mlc_s[k2] >= 'A' && mlc_s[k2] <= 'Z') ? (char)(mlc_s[k2] + 32) : mlc_s[k2];
          mlc[k2] = 0; }
        TClass *cls = NULL;
        if (e[0].t == T_OBJ) {
            TObj *o = (TObj *)e[0].u.p;
            if (o->h.kind == GC_KIND_EX || !o->cls) return false;
            cls = o->cls;
        } else if (e[0].t == T_STR) {
            extern TClass *class_resolve_lc(const char *fqcn_lc);
            char clc[256];
            const char *cs = tstr_val((TStr *)e[0].u.p);
            size_t cl = strlen(cs);
            if (cl >= sizeof clc) cl = sizeof clc - 1;
            for (size_t i = 0; i < cl; i++)
                clc[i] = (cs[i] >= 'A' && cs[i] <= 'Z') ? (char)(cs[i] + 32) : cs[i];
            clc[cl] = 0;
            cls = class_resolve_lc(clc);
            if (!cls) return false;
        } else {
            return false;
        }
        tphp_class_ensure_flat(cls);
        TMethod *m = tphp_class_find_method(cls, mlc);
        return m && (m->fn || m->native) && !m->is_abstract;
    }
    case T_OBJ: {
        TObj *o = (TObj *)v.u.p;
        if (o->h.kind == GC_KIND_EX || !o->cls) return false;
        tphp_class_ensure_flat(o->cls);
        TMethod *m = tphp_class_find_method(o->cls, "__invoke");
        return m && (m->fn || m->native) && !m->is_abstract && !m->is_static;
    }
    default:
        return false;
    }
}
static TVal bi_is_callable(VM *vm, TVal *args, int n) {
    if (n < 1) RAISE(vm, "ArgumentCountError", "is_callable() expects at least 1 argument");
    /* PHP signature: is_callable(value, syntax_only = false, &name = null) -
     * syntax_only is arg #2 (not #3) */
    bool syntax_only = (n >= 2) ? tval_to_bool(args[1]) : false;
    if (syntax_only) return tphp_vm_is_callable(args[0]) ? TV_TRUE : TV_FALSE;
    return k14_strict_callable(vm, args[0]) ? TV_TRUE : TV_FALSE;
}

/* time() */
static TVal bi_time(VM *vm, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return TV_INT((int64_t)time(NULL));
}

/* error_log($message): 写 stderr（CLI 对齐；bench 计时走这里，stdout 留 EXPECT） */
static TVal bi_error_log(VM *vm, TVal *args, int n) {
    if (n < 1)
        RAISE(vm, "ArgumentCountError", "error_log() expects at least 1 argument");
    TStr *s = tval_to_str(args[0]);
    fwrite(tstr_val(s), 1, tstr_len(s), stderr);
    fputc('\n', stderr);
    tval_release(&(TVal){ .u.p = s, .t = T_STR });
    return TV_NULL;
}

/* date(format[, timestamp=null]): 常用格式 Y/m/d/H/i/s/a/A/B/G/g/h/j/J/L/m/n/O/P/S/T/w/W/z/Z/U */
static TVal bi_date(VM *vm, TVal *args, int n) {
    if (n < 1 || n > 2)
        RAISE(vm, "ArgumentCountError", "date() expects 1 or 2 arguments");
    TStr *fmt = need_str(vm, &args[0], "date", "#1 ($format)");
    time_t ts = n >= 2 ? (time_t)tval_to_int(args[1]) : time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &ts);   /* 默认 UTC，与 PHP date 默认一致 */
#else
    gmtime_r(&ts, &tmv);
#endif
    /* PHP date 默认用 UTC（无 date_default_timezone_set 时） */
    char out[256];
    size_t o = 0;
    const char *p = tstr_val(fmt);
    const char *pe = p + tstr_len(fmt);
    while (p < pe && o < sizeof out - 8) {
        /* PHP date() 格式符无需 % 前缀：Y m d H 等直接识别 */
        char c = *p++;
        switch (c) {
        case 'Y': o += (size_t)snprintf(out + o, sizeof out - o, "%04d", tmv.tm_year + 1900); break;
        case 'y': o += (size_t)snprintf(out + o, sizeof out - o, "%02d", (tmv.tm_year + 1900) % 100); break;
        case 'm': o += (size_t)snprintf(out + o, sizeof out - o, "%02d", tmv.tm_mon + 1); break;
        case 'n': o += (size_t)snprintf(out + o, sizeof out - o, "%d", tmv.tm_mon + 1); break;
        case 'd': o += (size_t)snprintf(out + o, sizeof out - o, "%02d", tmv.tm_mday); break;
        case 'j': o += (size_t)snprintf(out + o, sizeof out - o, "%d", tmv.tm_mday); break;
        case 'H': o += (size_t)snprintf(out + o, sizeof out - o, "%02d", tmv.tm_hour); break;
        case 'G': o += (size_t)snprintf(out + o, sizeof out - o, "%d", tmv.tm_hour); break;
        case 'h': {
            int h = tmv.tm_hour % 12; if (h == 0) h = 12;
            o += (size_t)snprintf(out + o, sizeof out - o, "%02d", h);
            break;
        }
        case 'g': {
            int h = tmv.tm_hour % 12; if (h == 0) h = 12;
            o += (size_t)snprintf(out + o, sizeof out - o, "%d", h);
            break;
        }
        case 'i': o += (size_t)snprintf(out + o, sizeof out - o, "%02d", tmv.tm_min); break;
        case 's': o += (size_t)snprintf(out + o, sizeof out - o, "%02d", tmv.tm_sec); break;
        case 'a': case 'A':
            o += (size_t)snprintf(out + o, sizeof out - o, "%s",
                                  tmv.tm_hour < 12 ? (c == 'a' ? "am" : "AM") : (c == 'a' ? "pm" : "PM"));
            break;
        case 'B': { /* Swatch Beat */
            int beat = (int)(((ts % 86400) + 3600) % 86400 * 1000 / 86400);
            o += (size_t)snprintf(out + o, sizeof out - o, "%03d", beat);
            break;
        }
        case 'D': {
            static const char *wd[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", wd[tmv.tm_wday]);
            break;
        }
        case 'l': {
            static const char *wd[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", wd[tmv.tm_wday]);
            break;
        }
        case 'M': {
            static const char *mo[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", mo[tmv.tm_mon]);
            break;
        }
        case 'F': {
            static const char *mo[] = {"January","February","March","April","May","June",
                                       "July","August","September","October","November","December"};
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", mo[tmv.tm_mon]);
            break;
        }
        case 'w': o += (size_t)snprintf(out + o, sizeof out - o, "%d", tmv.tm_wday); break;
        case 'z': o += (size_t)snprintf(out + o, sizeof out - o, "%d", tmv.tm_yday); break;
        case 'U': o += (size_t)snprintf(out + o, sizeof out - o, "%lld", (long long)ts); break;
        case 'T': o += (size_t)snprintf(out + o, sizeof out - o, "UTC"); break;
        case 'P': o += (size_t)snprintf(out + o, sizeof out - o, "+00:00"); break;
        case 'O': o += (size_t)snprintf(out + o, sizeof out - o, "+0000"); break;
        case '\\': {
            if (p < pe) out[o++] = *p++;
            break;
        }
        default:
            out[o++] = c;
            break;
        }
    }
    out[o] = 0;
    return TV_STR(tstr_new(out, o));
}

/* mktime(hour, minute, second, month, day, year): UTC → unix ts */
static TVal bi_mktime(VM *vm, TVal *args, int n) {
    if (n != 6)
        RAISE(vm, "ArgumentCountError", "mktime() expects exactly 6 arguments");
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_hour = (int)need_int(vm, &args[0], "mktime", "#1 ($hour)");
    tmv.tm_min  = (int)need_int(vm, &args[1], "mktime", "#2 ($minute)");
    tmv.tm_sec  = (int)need_int(vm, &args[2], "mktime", "#3 ($second)");
    tmv.tm_mon  = (int)need_int(vm, &args[3], "mktime", "#4 ($month)") - 1;
    tmv.tm_mday = (int)need_int(vm, &args[4], "mktime", "#5 ($day)");
    tmv.tm_year = (int)need_int(vm, &args[5], "mktime", "#6 ($year)") - 1900;
    tmv.tm_isdst = -1;
#ifdef _WIN32
    return TV_INT((int64_t)_mkgmtime(&tmv));
#else
    return TV_INT((int64_t)timegm(&tmv));
#endif
}

/* checkdate(month, day, year) */
static TVal bi_checkdate(VM *vm, TVal *args, int n) {
    if (n != 3)
        RAISE(vm, "ArgumentCountError", "checkdate() expects exactly 3 arguments");
    int64_t mo = need_int(vm, &args[0], "checkdate", "#1 ($month)");
    int64_t dy = need_int(vm, &args[1], "checkdate", "#2 ($day)");
    int64_t yr = need_int(vm, &args[2], "checkdate", "#3 ($year)");
    if (mo < 1 || mo > 12 || yr < 1 || yr > 32767 || dy < 1) return TV_FALSE;
    static const int mdays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    int dim = mdays[mo];
    if (mo == 2 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0)) dim = 29;
    return dy <= dim ? TV_TRUE : TV_FALSE;
}

/* ---- M6 补齐：数组/字符串常用 ---- */

/* array_column(array, column_key[, index_key=null]) */
static TVal bi_array_column(VM *vm, TVal *args, int n) {
    if (n < 2 || n > 3)
        RAISE(vm, "ArgumentCountError", "array_column() expects 2 or 3 arguments");
    need_arr(vm, &args[0], "array_column", "#1 ($array)");
    TArr *a = (TArr *)args[0].u.p;
    TVal colk = args[1];
    TVal idxk = n >= 3 ? args[2] : TV_NULL;
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_ARR) continue;
        TArr *row = (TArr *)a->data[i].u.p;
        TVal *pv = tarr_get(row, colk);
        if (!pv) continue;
        TVal val = *pv;
        tval_addref(val);
        if (idxk.t != T_NULL) {
            TVal *ik = tarr_get(row, idxk);
            if (ik) {
                TVal key = *ik;
                tval_addref(key);
                tarr_set(out, key, val);
                tval_release(&key);
                continue;
            }
        }
        tarr_append(out, val);
    }
    return TV_ARR(out);
}

/* array_combine(keys[], values[]) */
static TVal bi_array_combine(VM *vm, TVal *args, int n) {
    if (n != 2)
        RAISE(vm, "ArgumentCountError", "array_combine() expects exactly 2 arguments");
    need_arr(vm, &args[0], "array_combine", "#1 ($keys)");
    need_arr(vm, &args[1], "array_combine", "#2 ($values)");
    TArr *ka = (TArr *)args[0].u.p;
    TArr *va = (TArr *)args[1].u.p;
    if (ka->n != va->n)
        RAISE(vm, "ValueError", "array_combine(): Argument #1 ($keys) and #2 ($values) must have the same number of elements");
    TArr *out = tarr_new();
    uint32_t vi = 0, ki = 0;
    for (uint32_t i = 0; i < ka->dused && ki < ka->n; i++) {
        if (ka->data[i].t == T_UNDEF) continue;
        /* 找 values 第 ki 个 */
        uint32_t seen = 0;
        TVal v = TV_NULL;
        for (uint32_t j = 0; j < va->dused; j++) {
            if (va->data[j].t == T_UNDEF) continue;
            if (seen == ki) { v = va->data[j]; break; }
            seen++;
        }
        TVal key = ka->data[i];
        tval_addref(key);
        tval_addref(v);
        tarr_set(out, key, v);
        tval_release(&key);
        ki++;
        (void)vi;
    }
    return TV_ARR(out);
}

/* array_chunk(array, size[, preserve_keys=false]) */
static TVal bi_array_chunk(VM *vm, TVal *args, int n) {
    if (n < 2 || n > 3)
        RAISE(vm, "ArgumentCountError", "array_chunk() expects 2 or 3 arguments");
    need_arr(vm, &args[0], "array_chunk", "#1 ($array)");
    int64_t sz = need_int(vm, &args[1], "array_chunk", "#2 ($length)");
    if (sz < 1)
        RAISE(vm, "ValueError", "array_chunk(): Argument #2 ($length) must be greater than 0");
    bool preserve = n >= 3 ? tval_to_bool(args[2]) : false;
    TArr *a = (TArr *)args[0].u.p;
    tarr_sync_buckets(a);
    uint32_t *inv = arr_key_inv_build(a);   /* 刀13：O(1) 键反查（原每元素 O(bcap) = 50k 137ms vs PHP 0.2ms） */
    TArr *out = tarr_new();
    TArr *cur = NULL;
    int64_t cnt = 0;
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        if (cnt % sz == 0) {
            cur = tarr_new();
            /* tarr_set/append 不 addref 值——out 持有 cur 的引用需手动 +1 */
            tarr_append(out, TV_ARR(cur));
            cur->h.rc++;   /* out 持有一份；本地指针也用 */
        }
        TVal key = arr_key_at_inv(a, i, inv);
        TVal v = a->data[i];
        tval_addref(v);
        if (preserve && key.t != T_NULL) {
            tval_addref(key);
            tarr_set(cur, key, v);
            tval_release(&key);
        } else {
            tarr_append(cur, v);
        }
        cnt++;
    }
    if (inv) tm_free(inv);
    return TV_ARR(out);
}

/* array_splice(array, offset[, length[, replacement]]): 移除+替换。
 * 刀14。PHP 8.5 实测语义：offset 负 = 距尾；length 负 = end = du+length、
 * 省略/NULL = 至末尾；replacement 标量包装单元素（实测 H）；返回移除段
 * **重排 list**（实测 E）；结果数组 = 前段原键 + replacement 展开（list 键
 * 顺延 / str 键保留，实测 F）+ 后段原键——**slot 整体替换**实现（构造新
 * TArr 后释放槽换入，免清空回填）。 */
static TVal bi_array_splice(VM *vm, TVal *args, int n) {
    if (n < 2 || n > 4)
        RAISE(vm, "ArgumentCountError", "array_splice() expects 2 to 4 arguments");
    need_arr(vm, &args[0], "array_splice", "#1 ($array)");
    int64_t offset = need_int(vm, &args[1], "array_splice", "#2 ($offset)");
    TArr *repl = NULL;
    TVal repl_val = TV_NULL;
    if (n >= 4) {
        if (args[3].t == T_ARR) {
            repl = (TArr *)args[3].u.p;          /* 借用 */
        } else {
            repl = tarr_new();
            tval_addref(args[3]);
            tarr_append(repl, args[3]);          /* 标量包装单元素（实测 H） */
            repl_val = TV_ARR(repl);             /* 本地持有 */
        }
    }
    TArr *a = (TArr *)args[0].u.p;
    tarr_sync_buckets(a);
    uint32_t du = a->dused;
    int64_t off = offset < 0 ? (int64_t)du + offset : offset;
    if (off < 0) off = 0;
    if (off > (int64_t)du) off = du;
    int64_t end;
    if (n < 3 || args[2].t == T_NULL) {
        end = du;
    } else {
        int64_t length = need_int(vm, &args[2], "array_splice", "#3 ($length)");
        end = length < 0 ? (int64_t)du + length : off + length;
        if (end < off) end = off;
        if (end > (int64_t)du) end = du;
    }
    uint32_t *inv = arr_key_inv_build(a);
    TArr *removed = tarr_new();
    TArr *newa = tarr_new();
    /* 键规则（PHP 8.5 实测）：**int 键全部重排连续（append）+ str 键保留
     * （set）**——适用于结果数组与 removed（E ret=[2,3] 重排 / F
     * ret={"b":2,"c":3} 保留）。 */
    for (uint32_t i = 0; i < (uint32_t)off; i++) {          /* 前段 */
        if (a->data[i].t == T_UNDEF) continue;
        TVal key = arr_key_at_inv(a, i, inv);
        tval_addref(a->data[i]);
        if (key.t == T_STR) { tarr_set(newa, key, a->data[i]); tval_release(&key); }
        else tarr_append(newa, a->data[i]);
    }
    if (repl)
        for (uint32_t j = 0; j < repl->dused; j++) {        /* replacement 展开 */
            if (repl->data[j].t == T_UNDEF) continue;
            tval_addref(repl->data[j]);
            tarr_append(newa, repl->data[j]);               /* list 键顺延 / str 键由源序保留——PHP 实测 F */
        }
    for (uint32_t i = (uint32_t)end; i < du; i++) {         /* 后段 */
        if (a->data[i].t == T_UNDEF) continue;
        TVal key = arr_key_at_inv(a, i, inv);
        tval_addref(a->data[i]);
        if (key.t == T_STR) { tarr_set(newa, key, a->data[i]); tval_release(&key); }
        else tarr_append(newa, a->data[i]);
    }
    for (uint32_t i = (uint32_t)off; i < (uint32_t)end; i++) {  /* 移除段 */
        if (a->data[i].t == T_UNDEF) continue;
        TVal key = arr_key_at_inv(a, i, inv);
        tval_addref(a->data[i]);
        if (key.t == T_STR) { tarr_set(removed, key, a->data[i]); tval_release(&key); }
        else tarr_append(removed, a->data[i]);
    }
    if (inv) tm_free(inv);
    if (repl_val.t == T_ARR) tval_release(&repl_val);
    /* 原地清空 + **存储字段整体转移**（不能 slot 换对象：内建参数槽 ≠
     * 调用方变量槽——bi_usort 清空回填同族；转移保序保键 100%、零
     * addref——引用随字段移交）。 */
    for (uint32_t i = 0; i < a->dused; i++)
        if (a->data[i].t != T_UNDEF) {
            tval_release(&a->data[i]);
            a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
        }
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX && a->buckets[b].key.t != T_NULL)
            tval_release(&a->buckets[b].key);
        a->buckets[b].key = TV_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    tm_free(a->data);
    if (a->buckets) tm_free(a->buckets);
    a->data = newa->data;
    a->buckets = newa->buckets;
    a->n = newa->n;
    a->bcap = newa->bcap;
    a->hmask = newa->hmask;
    a->next_idx = newa->next_idx;
    a->dcap = newa->dcap;
    a->dused = newa->dused;
    a->packed = newa->packed;
    a->bpending = newa->bpending;
    a->free_head = newa->free_head;
    newa->data = NULL;
    newa->buckets = NULL;
    newa->n = newa->bcap = newa->hmask = newa->dcap = newa->dused = 0;
    newa->packed = false;
    newa->bpending = 0;
    newa->next_idx = 0;
    newa->free_head = UINT32_MAX;
    tval_release(&TV_ARR(newa));    /* 空壳回收 */
    return TV_ARR(removed);
}

/* ucfirst(string): 首 ASCII alpha 大写（UTF-8 安全：>127 字节不动） */
static TVal bi_ucfirst(VM *vm, TVal *args, int n) {
    if (n != 1)
        RAISE(vm, "ArgumentCountError", "ucfirst() expects exactly 1 argument");
    TStr *s = need_str(vm, &args[0], "ucfirst", "#1 ($string)");
    if (s->len == 0 || s->c[0] < 'a' || s->c[0] > 'z') {
        TVal v = TV_STR(s);
        tval_addref(v);                      /* 借用返回：addref 平衡（keys 教训） */
        return v;
    }
    char *buf = tm_malloc(s->len);
    memcpy(buf, s->c, s->len);
    buf[0] = (char)(buf[0] - 0x20);
    TStr *out = tstr_new(buf, s->len);
    tm_free(buf);
    return TV_STR(out);
}

/* strtr(string, from, to) 或 strtr(string, replace_pairs[]) */
static TVal bi_strtr(VM *vm, TVal *args, int n) {
    if (n < 2 || n > 3)
        RAISE(vm, "ArgumentCountError", "strtr() expects 2 or 3 arguments");
    TStr *s = need_str(vm, &args[0], "strtr", "#1 ($string)");
    if (n == 2) {
        need_arr(vm, &args[1], "strtr", "#2 ($replace_pairs)");
        /* 简化：从长到短替换 */
        TArr *pairs = (TArr *)args[1].u.p;
        tarr_sync_buckets(pairs);
        /* 构造可变缓冲 */
        size_t cap = s->len + 1;
        char *buf = tm_malloc(cap);
        memcpy(buf, s->c, s->len + 1);
        size_t len = s->len;
        /* 收集 pairs */
        typedef struct { TStr *from, *to; } Pair;
        int np = 0;
        Pair *ps = tm_malloc(sizeof(Pair) * (pairs->n ? pairs->n : 1));
        for (uint32_t i = 0; i < pairs->dused; i++) {
            if (pairs->data[i].t == T_UNDEF) continue;
            TVal key = TV_NULL;
            for (uint32_t b = 0; b < pairs->bcap; b++)
                if (pairs->buckets[b].val_off == i && pairs->buckets[b].key.t == T_STR) {
                    key = pairs->buckets[b].key;
                    break;
                }
            if (key.t != T_STR || (pairs->data[i].t != T_STR)) continue;
            ps[np].from = (TStr *)key.u.p;
            ps[np].to = (TStr *)pairs->data[i].u.p;
            np++;
        }
        /* 简单冒泡按 from 长度降序 */
        for (int i = 0; i < np; i++)
            for (int j = i + 1; j < np; j++)
                if (ps[j].from->len > ps[i].from->len) {
                    Pair t = ps[i]; ps[i] = ps[j]; ps[j] = t;
                }
        /* 单遍扫描替换 */
        size_t out_n = 0;
        char *outb = tm_malloc(len * 4 + 16);   /* 最坏膨胀 */
        size_t i = 0;
        while (i < len) {
            bool hit = false;
            for (int k = 0; k < np; k++) {
                if (ps[k].from->len == 0) continue;
                if (i + ps[k].from->len <= len &&
                    memcmp(buf + i, ps[k].from->c, ps[k].from->len) == 0) {
                    memcpy(outb + out_n, ps[k].to->c, ps[k].to->len);
                    out_n += ps[k].to->len;
                    i += ps[k].from->len;
                    hit = true;
                    break;
                }
            }
            if (!hit) outb[out_n++] = buf[i++];
        }
        TStr *r = tstr_new(outb, out_n);
        tm_free(buf);
        tm_free(outb);
        tm_free(ps);
        return TV_STR(r);
    }
    /* 3 参数：from/to 字符映射 */
    TStr *from = need_str(vm, &args[1], "strtr", "#2 ($from)");
    TStr *to = need_str(vm, &args[2], "strtr", "#3 ($to)");
    size_t fl = from->len < to->len ? from->len : to->len;
    char *outb = tm_malloc(s->len + 1);
    for (size_t i = 0; i < s->len; i++) {
        char c = s->c[i];
        char nc = c;
        for (size_t k = 0; k < fl; k++)
            if (from->c[k] == c) { nc = to->c[k]; break; }
        outb[i] = nc;
    }
    outb[s->len] = 0;
    TStr *r = tstr_new(outb, s->len);
    tm_free(outb);
    return TV_STR(r);
}

/* ucwords(string) */
static TVal bi_ucwords(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "ucwords() expects exactly 1 argument");
    TStr *s = need_str(vm, &args[0], "ucwords", "#1 ($string)");
    char *b = tm_malloc(s->len + 1);
    memcpy(b, s->c, s->len + 1);
    bool start = true;
    for (size_t i = 0; i < s->len; i++) {
        if (b[i] >= 'a' && b[i] <= 'z' && start) b[i] = (char)(b[i] - 32);
        start = !(b[i] >= 'a' && b[i] <= 'z') && !(b[i] >= 'A' && b[i] <= 'Z') &&
                !(b[i] >= '0' && b[i] <= '9');
    }
    TStr *r = tstr_new(b, s->len);
    tm_free(b);
    return TV_STR(r);
}

/* bi_nl2br → ext/standard/standard.c */

/* strip_tags(string) — 去掉 HTML 标签 */
static TVal bi_strip_tags(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "strip_tags() expects exactly 1 argument");
    TStr *s = need_str(vm, &args[0], "strip_tags", "#1 ($string)");
    char *b = tm_malloc(s->len + 1);
    size_t o = 0;
    bool in_tag = false;
    for (size_t i = 0; i < s->len; i++) {
        char c = s->c[i];
        if (c == '<') { in_tag = true; continue; }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) b[o++] = c;
    }
    TStr *r = tstr_new(b, o);
    tm_free(b);
    return TV_STR(r);
}

/* htmlspecialchars(string): & < > " ' → 实体 */
static TVal bi_htmlspecialchars(VM *vm, TVal *args, int n) {
    if (n != 1) RAISE(vm, "ArgumentCountError", "htmlspecialchars() expects exactly 1 argument");
    TStr *s = need_str(vm, &args[0], "htmlspecialchars", "#1 ($string)");
    size_t cap = s->len * 6 + 1;
    char *b = tm_malloc(cap);
    size_t o = 0;
    for (size_t i = 0; i < s->len; i++) {
        char c = s->c[i];
        const char *rep = NULL;
        switch (c) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&#039;"; break;
        default: b[o++] = c; break;
        }
        if (rep) {
            size_t rl = strlen(rep);
            memcpy(b + o, rep, rl);
            o += rl;
        }
    }
    TStr *r = tstr_new(b, o);
    tm_free(b);
    return TV_STR(r);
}

/* explode 已有；array_fill(start_index, count, value) */
static TVal bi_array_fill(VM *vm, TVal *args, int n) {
    if (n != 3)
        RAISE(vm, "ArgumentCountError", "array_fill() expects exactly 3 arguments");
    int64_t start = need_int(vm, &args[0], "array_fill", "#1 ($start_index)");
    int64_t cnt = need_int(vm, &args[1], "array_fill", "#2 ($count)");
    if (cnt < 0)
        RAISE(vm, "ValueError", "array_fill(): Argument #2 ($count) must be greater than or equal to 0");
    TArr *out = tarr_new();
    TVal v = args[2];
    tval_addref(v);
    for (int64_t i = 0; i < cnt; i++) {
        TVal key = TV_INT(start + i);
        tval_addref(key);
        tarr_set(out, key, v);
        tval_release(&key);
    }
    tval_release(&v);
    return TV_ARR(out);
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
    { "is_a", bi_is_a },
    { "is_subclass_of", bi_is_subclass_of },
    { "get_parent_class", bi_get_parent_class },
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
    { "asort", bi_asort },
    { "arsort", bi_arsort },
    { "ksort", bi_ksort },
    { "krsort", bi_krsort },
    /* M6-b: callable 数组 + 常用补齐 */
    { "array_map", bi_array_map },
    { "array_filter", bi_array_filter },
    { "array_reduce", bi_array_reduce },
    { "usort", bi_usort },
    { "array_flip", bi_array_flip },
    { "array_unique", bi_array_unique },
    { "array_search", bi_array_search },
    { "array_key_first", bi_array_key_first },
    { "array_key_last", bi_array_key_last },
    { "range", bi_range },
    { "chr", bi_chr },
    { "ord", bi_ord },
    { "strcmp", bi_strcmp },
    { "strncmp", bi_strncmp },
    { "strcasecmp", bi_strcasecmp },
    { "strncasecmp", bi_strncasecmp },
    { "function_exists", bi_function_exists },
    { "ucfirst", bi_ucfirst },
    { "array_diff", bi_array_diff },
    { "array_intersect", bi_array_intersect },
    { "array_count_values", bi_array_count_values },
    { "array_splice", bi_array_splice },
    { "call_user_func", bi_call_user_func },
    { "call_user_func_array", bi_call_user_func_array },
    { "is_callable", bi_is_callable },
    { "time", bi_time },
    { "error_log", bi_error_log },
    { "date", bi_date },
    { "mktime", bi_mktime },
    { "checkdate", bi_checkdate },
    { "array_column", bi_array_column },
    { "array_combine", bi_array_combine },
    { "array_chunk", bi_array_chunk },
    { "array_fill", bi_array_fill },
    { "strtr", bi_strtr },
    { "ucwords", bi_ucwords },
    { "nl2br", bi_nl2br },
    { "strip_tags", bi_strip_tags },
    { "htmlspecialchars", bi_htmlspecialchars },
    /* strings */
    { "substr", bi_substr },
    { "str_replace", bi_str_replace },
    { "strpos", bi_strpos },
    { "stripos", bi_stripos },
    { "strrpos", bi_strrpos },
    { "strripos", bi_strripos },
    { "basename", bi_basename },
    { "dirname", bi_dirname },
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
    /* M6-a: JSON（json.c） */
    { "json_encode", bi_json_encode },
    { "json_decode", bi_json_decode },
    { "json_last_error", bi_json_last_error },
    { "json_last_error_msg", bi_json_last_error_msg },
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
    /* Tphp\ 命名空间：协程 API 的唯一入口（async(EXPR) 的调用形态由
     * parser 改写为 Tphp\spawn(fn() => EXPR)，不经过这里的 async 表项） */
    { "Tphp\\spawn", tphp_async_spawn },
    { "Tphp\\async", tphp_async_wrap },
    { "Tphp\\await", tphp_async_await },
    { "Tphp\\suspend", tphp_async_suspend },
    { "Tphp\\current", tphp_async_current },
    { "Tphp\\all", tphp_async_all },
    { "Tphp\\any", tphp_async_any },
    { "Tphp\\run", tphp_async_run },
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
    { "is_file", tphp_io_is_file },
    { "is_dir", tphp_io_is_dir },
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
    /* 包管理（pkg.c；tphp new/add/... 的 PHP 可调用层） */
    { "pkg_new", bi_pkg_new },
    { "pkg_add", bi_pkg_add },
    { "pkg_remove", bi_pkg_remove },
    { "pkg_install", bi_pkg_install },
    /* 正则（ext/pcre；PLAN/preg.md） */
    { "preg_match", bi_preg_match },
    { "preg_match_all", bi_preg_match_all },
    { "preg_replace", bi_preg_replace },
    { "preg_split", bi_preg_split },
    { "preg_grep", bi_preg_grep },
    { "preg_last_error", bi_preg_last_error },
    { "preg_last_error_msg", bi_preg_last_error_msg },
    { "preg_quote", bi_preg_quote },
};

const int tphp_builtin_count = (int)(sizeof tphp_builtins / sizeof tphp_builtins[0]);
