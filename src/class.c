/*
 * TinyPHP.rt - class.c - class registry, flattening, member lookup, objects
 *
 * M3 scope: class/interface (trait/enum binding included in flatten, with
 * conflict rules applied by the compiler into the TClass use tables).
 * Class names are case-insensitive (PHP semantics): the registry keys on
 * lowercased FQCNs but stores the declared spelling.
 */
#include "class.h"
#include "tphp.h"
#include "ast.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- case-insensitive helpers (ASCII; PHP identifiers are ASCII for
 * case-folding purposes even when the source is UTF-8) ---- */
static char lc_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool tstr_case_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (lc_ascii(*a) != lc_ascii(*b)) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void lc_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = lc_ascii(src[i]);
    dst[i] = 0;
}

/* ---- P1-1: type declaration checks ---- */

/* PHP-style value type name for error messages ("string given" etc.) */
const char *tphp_val_type_name(TVal v) {
    switch (v.t) {
    case T_NULL:   return "null";
    case T_TRUE:
    case T_FALSE:  return "bool";
    case T_INT:    return "int";
    case T_FLOAT:  return "float";
    case T_STR:    return "string";
    case T_ARR:    return "array";
    case T_CLOSURE: return "Closure";
    case T_OBJ: {
        TClass *c = ((TObj *)v.u.p)->cls;
        return c && c->name ? tstr_val(c->name) : "object";
    }
    default:       return "null";   /* T_UNDEF: uninitialized */
    }
}

static bool type_seg_ok(const char *seg, size_t len, TVal v, TClass *scope) {
    /* builtin spellings */
    if (len == 3 && memcmp(seg, "int", 3) == 0)   return v.t == T_INT;
    if (len == 5 && memcmp(seg, "float", 5) == 0) return v.t == T_FLOAT || v.t == T_INT;
    if (len == 6 && memcmp(seg, "string", 6) == 0) return v.t == T_STR;
    if (len == 4 && memcmp(seg, "bool", 4) == 0)  return v.t == T_TRUE || v.t == T_FALSE;
    if (len == 5 && memcmp(seg, "array", 5) == 0) return v.t == T_ARR;
    if (len == 6 && memcmp(seg, "object", 6) == 0) return v.t == T_OBJ;
    if (len == 8 && memcmp(seg, "iterable", 8) == 0) return v.t == T_ARR || v.t == T_OBJ;
    if (len == 5 && memcmp(seg, "mixed", 5) == 0) return true;
    if (len == 4 && memcmp(seg, "null", 4) == 0)  return v.t == T_NULL;
    if (len == 4 && memcmp(seg, "true", 4) == 0)  return v.t == T_TRUE;
    if (len == 5 && memcmp(seg, "false", 5) == 0) return v.t == T_FALSE;
    if (len == 8 && memcmp(seg, "callable", 8) == 0) return v.t == T_CLOSURE || v.t == T_STR;
    if (len == 7 && memcmp(seg, "closure", 7) == 0) return v.t == T_CLOSURE;
    if (len == 4 && memcmp(seg, "self", 4) == 0)
        return scope && v.t == T_OBJ && tphp_class_isa(((TObj *)v.u.p)->cls, scope);
    if (len == 6 && memcmp(seg, "static", 6) == 0)
        return scope && v.t == T_OBJ && tphp_class_isa(((TObj *)v.u.p)->cls, scope);
    if (len == 6 && memcmp(seg, "parent", 6) == 0)
        return scope && scope->parent && v.t == T_OBJ &&
               tphp_class_isa(((TObj *)v.u.p)->cls, scope->parent);
    /* class/interface name (case-insensitive, leading '\' allowed) */
    {
        char lc[256];
        size_t k = 0;
        const char *s = seg;
        if (*s == '\\') s++;
        for (; s[k] && k + 1 < sizeof lc; k++) lc[k] = lc_ascii(s[k]);
        lc[k] = 0;
        TClass *want = tphp_class_lookup(lc);
        if (!want) return false;
        return v.t == T_OBJ && tphp_class_isa(((TObj *)v.u.p)->cls, want);
    }
}

bool tphp_type_accepts(const char *type, TVal v, TClass *scope,
                       const char **given) {
    if (!type || !*type) return true;
    const char *p = type;
    bool nullable = false;
    if (*p == '?') { nullable = true; p++; }
    for (;;) {
        const char *seg = p;
        while (*p && *p != '|') p++;
        size_t len = (size_t)(p - seg);
        if (len && type_seg_ok(seg, len, v, scope)) return true;
        if (!*p) break;
        p++;   /* skip '|' */
    }
    if (nullable && v.t == T_NULL) return true;
    if (given) *given = tphp_val_type_name(v);
    return false;
}

/* ---- registry: open-addressed table keyed on lowercased FQCN ----
 * M5: cls_tab is per-OS-thread (_Thread_local). The master table (process
 * level, written only at compile time) is the clone source for worker
 * threads: tphp_class_tab_clone_thread() deep-copies every TClass into the
 * calling thread's registry so runtime mutable state (static property
 * values, enum singletons) never crosses threads. */
#define CLS_TAB_INIT 64

typedef struct {
    char *key_lc;      /* malloc'd lowercase FQCN */
    TClass *cls;
} ClsSlot;

static _Thread_local ClsSlot *cls_tab;
static _Thread_local uint32_t cls_cap, cls_n;

/* master registry: process level, frozen after compilation */
static ClsSlot *cls_tab_master;
static uint32_t cls_cap_m, cls_n_m;

/* B4: 声明序记录（定义在 ClsRt 区，注册函数在其之前） */
static void cls_ord_push(TClass *cls);

static uint32_t cls_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static void cls_tab_grow(void) {
    uint32_t ncap = cls_cap ? cls_cap * 2 : CLS_TAB_INIT;
    ClsSlot *nt = tm_malloc(sizeof(ClsSlot) * ncap);
    memset(nt, 0, sizeof(ClsSlot) * ncap);
    for (uint32_t i = 0; i < cls_cap; i++) {
        if (!cls_tab[i].cls) continue;
        uint32_t j = cls_hash(cls_tab[i].key_lc) & (ncap - 1);
        while (nt[j].cls) j = (j + 1) & (ncap - 1);
        nt[j] = cls_tab[i];
    }
    tm_free(cls_tab);
    cls_tab = nt;
    cls_cap = ncap;
}

/* master 表写入（仅编译期主线程发生；克隆源） */
static void cls_tab_master_put(TClass *cls) {
    if (cls_n_m + 1 >= cls_cap_m - cls_cap_m / 4) {
        uint32_t ncap = cls_cap_m ? cls_cap_m * 2 : CLS_TAB_INIT;
        ClsSlot *nt = tm_malloc(sizeof(ClsSlot) * ncap);
        memset(nt, 0, sizeof(ClsSlot) * ncap);
        for (uint32_t i = 0; i < cls_cap_m; i++) {
            if (!cls_tab_master[i].cls) continue;
            uint32_t j = cls_hash(cls_tab_master[i].key_lc) & (ncap - 1);
            while (nt[j].cls) j = (j + 1) & (ncap - 1);
            nt[j] = cls_tab_master[i];
        }
        tm_free(cls_tab_master);
        cls_tab_master = nt;
        cls_cap_m = ncap;
    }
    char lc[256];
    lc_copy(lc, tstr_val(cls->name), sizeof lc);
    uint32_t j = cls_hash(lc) & (cls_cap_m - 1);
    while (cls_tab_master[j].cls) j = (j + 1) & (cls_cap_m - 1);
    cls_tab_master[j].key_lc = tm_malloc(strlen(lc) + 1);
    strcpy(cls_tab_master[j].key_lc, lc);
    cls_tab_master[j].cls = cls;
    cls_n_m++;
}

/* 编译期注册：双写 master（克隆源）+ 本线程表。仅主线程编译期调用。 */
bool tphp_class_register_compile(TClass *cls) {
    cls_tab_master_put(cls);
    return tphp_class_register(cls);
}

/* 运行期注册（内部类 Tphp\Coroutine 等）：只进本线程表 */
bool tphp_class_register(TClass *cls) {
    if (cls_n + 1 >= cls_cap - cls_cap / 4) cls_tab_grow();
    char lc[256];
    lc_copy(lc, tstr_val(cls->name), sizeof lc);
    uint32_t j = cls_hash(lc) & (cls_cap - 1);
    while (cls_tab[j].cls) {
        if (strcmp(cls_tab[j].key_lc, lc) == 0) return false;  /* duplicate */
        j = (j + 1) & (cls_cap - 1);
    }
    cls_tab[j].key_lc = tm_malloc(strlen(lc) + 1);
    strcpy(cls_tab[j].key_lc, lc);
    cls_tab[j].cls = cls;
    cls_n++;
    cls_ord_push(cls);          /* B4: 注册序（= 声明序）记录 */
    return true;
}

TClass *tphp_class_lookup(const char *fqcn_lc) {
    if (!cls_cap) return NULL;
    uint32_t j = cls_hash(fqcn_lc) & (cls_cap - 1);
    while (cls_tab[j].cls) {
        if (strcmp(cls_tab[j].key_lc, fqcn_lc) == 0) return cls_tab[j].cls;
        j = (j + 1) & (cls_cap - 1);
    }
    return NULL;
}

bool tphp_class_exists(const char *fqcn_lc) {
    return tphp_class_lookup(fqcn_lc) != NULL;
}

/* ---- per-thread class runtime state (M5) ----
 * Static property values and enum singletons live in a side table keyed by
 * TClass*, never in the (shared, frozen) TClass itself. First access seeds
 * the slots from the class's frozen template with a deep copy — heap
 * objects inside default values (array literals folded at compile time)
 * are therefore per-thread too. */
typedef struct ClsRt {
    TClass *cls;
    TVal *sprop_vals;      /* n_sprops+1 slots */
    int n;                 /* B4: seeded slot count (release bound) */
    TObj **case_objs;      /* n_cases slots (enum singletons) */
    struct ClsRt *next;
} ClsRt;

static _Thread_local ClsRt *g_cls_rt;

/* B4: 类注册序（声明序）——hash 表无序，shutdown 按此序释放静态属性，
 * 级联析构顺序对齐 PHP（q4e: d1,d2 而非 gc_list 逆创建序 d2,d1） */
static _Thread_local TClass **cls_ord;
static _Thread_local uint32_t cls_ord_n, cls_ord_cap;

static void cls_ord_push(TClass *cls) {
    if (cls_ord_n == cls_ord_cap) {
        cls_ord_cap = cls_ord_cap ? cls_ord_cap * 2 : 32;
        cls_ord = tm_realloc(cls_ord, sizeof(TClass *) * (size_t)cls_ord_cap);
    }
    cls_ord[cls_ord_n++] = cls;
}

static ClsRt *cls_rt_peek(TClass *cls) {
    for (ClsRt *r = g_cls_rt; r; r = r->next)
        if (r->cls == cls) return r;
    return NULL;
}

static ClsRt *cls_rt_get(TClass *cls) {
    ClsRt *r = cls_rt_peek(cls);
    if (r) return r;
    r = tm_malloc(sizeof *r);
    r->cls = cls;
    r->sprop_vals = tm_malloc(sizeof(TVal) *
                              (size_t)(cls->n_sprops ? cls->n_sprops : 1));
    /* seed from the frozen template (deep copy: per-thread heap values) */
    for (int i = 0; i < cls->n_sprops; i++)
        r->sprop_vals[i] = tval_clone_deep(cls->sprop_vals[i]);
    r->n = cls->n_sprops;
    r->case_objs = tm_malloc(sizeof(TObj *) *
                             (size_t)(cls->n_cases ? cls->n_cases : 1));
    for (int i = 0; i < cls->n_cases; i++) r->case_objs[i] = NULL;
    r->next = g_cls_rt;
    g_cls_rt = r;
    return r;
}

/* B4: 请求收尾——按类声明序释放本线程静态属性值，持有链级联触发
 * __destruct（对齐 PHP shutdown 序）。共享 TClass.cvals 不在此释放：
 * 值为编译期常量（无析构面）、且 cvals 是 worker 共享写入位，释放会
 * 破坏其它线程——保持 program 生命周期（等价 PHP 请求生命周期）。
 * 幂等：释放后置 T_UNDEF，重复收尾为 no-op。 */
void tphp_class_release_sprops(void) {
    for (uint32_t i = 0; i < cls_ord_n; i++) {
        ClsRt *r = cls_rt_peek(cls_ord[i]);
        if (!r) continue;                     /* 从未访问过 → 值仍在模板 */
        for (int k = 0; k < r->n; k++) {
            tval_release(&r->sprop_vals[k]);
            r->sprop_vals[k] = (TVal){ .u.i = 0, .t = T_UNDEF };
        }
    }
}

TVal *tphp_class_sprop_vals(TClass *cls) {
    return cls_rt_get(cls)->sprop_vals;
}

/* ---- autoload hook (spl_autoload_register): stores a PHP-level callable
 * (TClosure or function-name string) run by the VM on class-lookup miss ---- */
static _Thread_local TVal autoload_fn = { .u.i = 0, .t = T_NULL };

void tphp_class_set_autoload_val(TVal fn) {
    tval_addref(fn);
    tval_release(&autoload_fn);
    autoload_fn = fn;
}

/* resolve a class by lowercase FQCN; runs the registered autoloader once on
 * miss (runtime only — during load there is no VM to run PHP code in).
 * orig_name（可 NULL）以原始大小写传给 spl_autoload 回调（PHP 对齐）。 */
TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name) {
    TClass *c = tphp_class_lookup(fqcn_lc);
    if (c) return c;
    if (autoload_fn.t != T_NULL && tphp_vm_current) {
        const char *on = orig_name ? orig_name : fqcn_lc;
        TStr *req = tstr_intern(on, strlen(on));
        tval_addref(TV_STR(req));
        tphp_vm_run_autoloader(autoload_fn, req);
        return tphp_class_lookup(fqcn_lc);
    }
    return NULL;
}

TClass *class_resolve_lc(const char *fqcn_lc) {
    return class_resolve_lc2(fqcn_lc, NULL);
}

/* ---- flattening ---- */

static int16_t magic_index(const char *lc) {
    if (strcmp(lc, "__construct") == 0)   return -3;
    if (strcmp(lc, "__destruct") == 0)    return -4;
    if (strcmp(lc, "__get") == 0)         return -5;
    if (strcmp(lc, "__set") == 0)         return -6;
    if (strcmp(lc, "__isset") == 0)       return -7;
    if (strcmp(lc, "__unset") == 0)       return -8;
    if (strcmp(lc, "__call") == 0)        return -9;
    if (strcmp(lc, "__callstatic") == 0)  return -10;
    if (strcmp(lc, "__tostring") == 0)    return -11;
    if (strcmp(lc, "__invoke") == 0)      return -12;
    if (strcmp(lc, "__clone") == 0)       return -13;
    if (strcmp(lc, "__debuginfo") == 0)   return -14;
    if (strcmp(lc, "__clonestate") == 0)  return -15;
    return -2;   /* ordinary method */
}

/* map magic_index sentinel back into the TClass quick-link slot */
static int16_t *quick_slot(TClass *cls, int16_t code) {
    switch (code) {
    case -3:  return &cls->m_ctor;
    case -4:  return &cls->m_dtor;
    case -5:  return &cls->m_get;
    case -6:  return &cls->m_set;
    case -7:  return &cls->m_isset;
    case -8:  return &cls->m_unset;
    case -9:  return &cls->m_call;
    case -10: return &cls->m_callstatic;
    case -11: return &cls->m_tostring;
    case -12: return &cls->m_invoke;
    case -13: return &cls->m_clone;
    case -14: return &cls->m_debuginfo;
    case -15: return &cls->m_clonestate;
    }
    return NULL;
}

/* ---- P3: override 签名校验的类型格 ----
 * 段级值域包含关系：covers(wide, narrow) = wide 的值域 ⊇ narrow 的值域。
 * 精确相等 / mixed / int⊂float / array⊂iterable / 类名⊂object / 类层级 isa。
 * 参数逆变：child 参数类型必须 ⊇ parent 参数类型（每个 parent 段被 child
 * 某段覆盖，且 nullability 收缩禁止）；返回协变：child 返回类型 ⊆ parent。 */
static bool type_seg_is_class(const char *s) {
    static const char *builtins[] = {
        "int", "float", "string", "bool", "array", "object", "mixed",
        "null", "true", "false", "callable", "iterable", "void", "never",
        "self", "static", "parent", "closure",
    };
    for (size_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (strcmp(s, builtins[i]) == 0) return false;
    return true;
}

static bool type_seg_covers(const char *wide, const char *narrow, TClass *scope) {
    (void)scope;
    if (strcmp(wide, narrow) == 0) return true;
    if (strcmp(wide, "mixed") == 0) return true;
    if (strcmp(narrow, "mixed") == 0) return false;
    if (strcmp(wide, "float") == 0 && strcmp(narrow, "int") == 0) return true;
    if (strcmp(wide, "iterable") == 0 && strcmp(narrow, "array") == 0) return true;
    if (type_seg_is_class(wide)) {
        if (strcmp(narrow, "object") == 0) return true;
        if (!type_seg_is_class(narrow)) return false;
        /* 类层级：narrow 类 isa wide 类 */
        char wlc[256], nlc[256];
        lc_copy(wlc, wide, sizeof wlc);
        lc_copy(nlc, narrow, sizeof nlc);
        TClass *wc = tphp_class_lookup(wlc);
        TClass *nc = tphp_class_lookup(nlc);
        if (!wc || !nc) return strcmp(wide, narrow) == 0;
        return tphp_class_isa(nc, wc);
    }
    return false;
}

/* 段迭代辅助：跳过开头的 '?'（nullable 标记由调用方单独处理） */
static const char *type_skip_q(const char *t) {
    return t[0] == '?' ? t + 1 : t;
}

/* 参数逆变：child 的类型必须接受 parent 类型的全部值 */
static bool type_param_contravariant(const char *child, const char *parent,
                                     TClass *scope) {
    if (!parent || !*parent) return !child || !*child;   /* parent mixed */
    if (!child || !*child) return true;                  /* child mixed ⊇ 一切 */
    if (strcmp(child, parent) == 0) return true;
    bool pnull = parent[0] == '?';
    bool cnull = child[0] == '?';
    if (pnull && !cnull) return false;                   /* null 必须仍被接受 */
    /* 逐 parent 段：必须被 child 的某段覆盖（段数小，双重循环） */
    const char *p = type_skip_q(parent);
    for (;;) {
        const char *seg = p;
        while (*p && *p != '|') p++;
        size_t len = (size_t)(p - seg);
        char pbuf[160];
        if (len >= sizeof pbuf) len = sizeof pbuf - 1;
        memcpy(pbuf, seg, len);
        pbuf[len] = 0;
        bool covered = false;
        const char *q = type_skip_q(child);
        for (;;) {
            const char *cseg = q;
            while (*q && *q != '|') q++;
            size_t clen = (size_t)(q - cseg);
            char cbuf[160];
            if (clen >= sizeof cbuf) clen = sizeof cbuf - 1;
            memcpy(cbuf, cseg, clen);
            cbuf[clen] = 0;
            if (type_seg_covers(cbuf, pbuf, scope)) { covered = true; break; }
            if (!*q) break;
            q++;
        }
        if (!covered) return false;
        if (!*p) break;
        p++;
    }
    return true;
}

/* 返回协变：child 的返回类型必须 ⊆ parent 的返回类型 */
static bool type_ret_covariant(const char *child, const char *parent,
                               TClass *scope) {
    if (!parent || !*parent) return true;                /* parent 未声明 */
    if (!child || !*child) return false;                 /* child 省略 → fatal */
    if (strcmp(child, parent) == 0) return true;
    if (strcmp(parent, "mixed") == 0) return true;
    if (strcmp(child, "mixed") == 0) return false;
    bool cnull = child[0] == '?';
    bool pnull = parent[0] == '?';
    if (cnull && !pnull) return false;                   /* child 可能返回 null */
    const char *c = type_skip_q(child);
    for (;;) {
        const char *cseg = c;
        while (*c && *c != '|') c++;
        size_t len = (size_t)(c - cseg);
        char cbuf[160];
        if (len >= sizeof cbuf) len = sizeof cbuf - 1;
        memcpy(cbuf, cseg, len);
        cbuf[len] = 0;
        bool covered = false;
        const char *p = type_skip_q(parent);
        for (;;) {
            const char *pseg = p;
            while (*p && *p != '|') p++;
            size_t plen = (size_t)(p - pseg);
            char pbuf[160];
            if (plen >= sizeof pbuf) plen = sizeof pbuf - 1;
            memcpy(pbuf, pseg, plen);
            pbuf[plen] = 0;
            if (type_seg_covers(pbuf, cbuf, scope)) { covered = true; break; }
            if (!*p) break;
            p++;
        }
        if (!covered) return false;
        if (!*c) break;
        c++;
    }
    return true;
}

static void add_method(TClass *cls, TMethod m) {
    char lc[128];
    lc_copy(lc, tstr_val(m.name), sizeof lc);
    /* override check within the flat table being built */
    for (int i = 0; i < cls->n_methods; i++) {
        char olc[128];
        lc_copy(olc, tstr_val(cls->methods[i].name), sizeof olc);
        if (strcmp(olc, lc) == 0) {
            /* child replaces parent; visibility may not be reduced and a
             * final method may not be overridden */
            if (cls->methods[i].visibility == CVIS_PRIVATE ||
                (m.visibility != CVIS_PUBLIC &&
                 cls->methods[i].visibility == CVIS_PUBLIC))
                tphp_fatal_src("class.c", 0,
                    "Access level of an overriding method must be compatible");
            if (cls->methods[i].is_final)
                tphp_fatal_src("class.c", 0,
                    "Final method %s::%s() cannot be overridden",
                    tstr_val(cls->name), tstr_val(cls->methods[i].name));
            /* P3: override 签名校验——__construct 豁免（PHP 同）；抽象/
             * 接口声明（fn NULL，无签名可查）与内部 native 跳过 */
            TFunc *pf = cls->methods[i].fn, *cf = m.fn;
            if (strcmp(olc, "__construct") != 0 && pf && cf) {
                const char *pcls = cls->parent ? tstr_val(cls->parent->name) : "?";
                if (cls->methods[i].is_static != m.is_static)
                    tphp_fatal_src("class.c", 0,
                        cls->methods[i].is_static
                            ? "Cannot make static method %s::%s() non static in class %s"
                            : "Cannot make non static method %s::%s() static in class %s",
                        pcls, tstr_val(cls->methods[i].name), tstr_val(cls->name));
                /* 参数：只增不减（variadic 父类豁免）；类型逆变 */
                if (!pf->is_variadic && cf->n_params < pf->n_params)
                    tphp_fatal_src("class.c", 0,
                        "Declaration of %s::%s() must be compatible with %s::%s()",
                        tstr_val(cls->name), tstr_val(m.name),
                        pcls, tstr_val(cls->methods[i].name));
                for (int k = 0; k < pf->n_params && k < cf->n_params; k++) {
                    const char *pt = (pf->param_types && pf->param_types[k])
                        ? tstr_val(pf->param_types[k]) : NULL;
                    const char *ct = (cf->param_types && cf->param_types[k])
                        ? tstr_val(cf->param_types[k]) : NULL;
                    if (!type_param_contravariant(ct, pt, cls))
                        tphp_fatal_src("class.c", 0,
                            "Declaration of %s::%s() must be compatible with %s::%s()",
                            tstr_val(cls->name), tstr_val(m.name),
                            pcls, tstr_val(cls->methods[i].name));
                }
                /* 返回类型协变 */
                const char *prt = pf->ret_type ? tstr_val(pf->ret_type) : NULL;
                const char *crt = cf->ret_type ? tstr_val(cf->ret_type) : NULL;
                if (!type_ret_covariant(crt, prt, cls))
                    tphp_fatal_src("class.c", 0,
                        "Declaration of %s::%s() must be compatible with %s::%s()",
                        tstr_val(cls->name), tstr_val(m.name),
                        pcls, tstr_val(cls->methods[i].name));
            }
            cls->methods[i] = m;
            return;
        }
    }
    cls->methods = tm_realloc(cls->methods,
                              sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    cls->methods[cls->n_methods++] = m;
}

static void add_prop(TClass *cls, TProp p) {
    for (int i = 0; i < cls->n_props; i++) {
        if (tstr_case_eq(tstr_val(cls->props[i].name), tstr_val(p.name))) {
            /* PHP 8.4：final 钩子不得被子类覆盖 */
            if (cls->props[i].hook_get_final && p.hook_get)
                tphp_fatal_src("class.c", 0,
                               "Cannot override final get hook of property %s::$%s",
                               tstr_val(cls->props[i].decl ? cls->props[i].decl->name
                                                           : cls->name),
                               tstr_val(p.name));
            if (cls->props[i].hook_set_final && p.hook_set)
                tphp_fatal_src("class.c", 0,
                               "Cannot override final set hook of property %s::$%s",
                               tstr_val(cls->props[i].decl ? cls->props[i].decl->name
                                                           : cls->name),
                               tstr_val(p.name));
            /* child redeclares parent prop: keep parent slot, update meta */
            p.slot = cls->props[i].slot;
            cls->props[i] = p;
            return;
        }
    }
    p.slot = (uint16_t)cls->n_props;
    cls->props = tm_realloc(cls->props, sizeof(TProp) * (size_t)(cls->n_props + 1));
    cls->props[cls->n_props++] = p;
}

static void add_sprop(TClass *cls, TProp p) {
    for (int i = 0; i < cls->n_sprops; i++) {
        if (tstr_case_eq(tstr_val(cls->sprops[i].name), tstr_val(p.name))) {
            /* P2-B: 子类重声明静态属性 = 独立存储槽（PHP 语义）——
             * A::$x 与 B::$x 是两个变量。此前沿用父类槽：子类声明值丢失、
             * 写子类会串改父类。flat 表条目仍替换（名字查找命中子类的
             * 可见性/类型/默认值），但槽位指向新空间。 */
            p.slot = (uint16_t)cls->n_sprops;
            cls->sprops = tm_realloc(cls->sprops, sizeof(TProp) * (size_t)(cls->n_sprops + 1));
            if (cls->sprop_vals)
                cls->sprop_vals = tm_realloc(cls->sprop_vals,
                    sizeof(TVal) * (size_t)(cls->n_sprops + 1));
            else
                cls->sprop_vals = tm_malloc(sizeof(TVal) * (size_t)(cls->n_sprops + 1));
            TVal dv = p.defval.t == T_UNDEF ? (TVal){ .u.i = 0, .t = T_NULL } : p.defval;
            tval_addref(dv);
            cls->sprop_vals[cls->n_sprops] = dv;
            cls->sprops[i] = p;
            cls->n_sprops++;
            return;
        }
    }
    p.slot = (uint16_t)cls->n_sprops;
    cls->sprops = tm_realloc(cls->sprops, sizeof(TProp) * (size_t)(cls->n_sprops + 1));
    if (cls->sprop_vals)
        cls->sprop_vals = tm_realloc(cls->sprop_vals,
            sizeof(TVal) * (size_t)(cls->n_sprops + 1));
    else
        cls->sprop_vals = tm_malloc(sizeof(TVal) * (size_t)(cls->n_sprops + 1));
    /* seed the value slot from the declaration default (UNDEF -> NULL) */
    TVal dv = p.defval.t == T_UNDEF ? (TVal){ .u.i = 0, .t = T_NULL } : p.defval;
    tval_addref(dv);
    cls->sprop_vals[cls->n_sprops] = dv;
    cls->sprops[cls->n_sprops++] = p;
}

static void add_iface_name(TClass *cls, TStr *name) {
    for (int i = 0; i < cls->n_iface_names; i++)
        if (cls->iface_names[i] == name) return;
    cls->iface_names = tm_realloc(cls->iface_names,
                                  sizeof(TStr *) * (size_t)(cls->n_iface_names + 1));
    cls->iface_names[cls->n_iface_names++] = name;
}

/* ---- trait binding (PHP 8.5: traits bind after the parent) ----
 * as/insteadof rules come from the compiler's AModList (ast.h). */
static void bind_traits(TClass *cls) {
    if (!cls->n_trait_names) return;
    AModList *mods = (AModList *)cls->trait_mods;

    for (int ti = 0; ti < cls->n_trait_names; ti++) {
        char tlc[256];
        lc_copy(tlc, tstr_val(cls->trait_names[ti]), sizeof tlc);
        TClass *t = class_resolve_lc2(tlc, tstr_val(cls->trait_names[ti]));
        if (!t || t->kind != CK_TRAIT)
            tphp_fatal_src("class.c", 0, "Trait \"%s\" not found",
                           tstr_val(cls->trait_names[ti]));
        for (int mi = 0; mi < t->n_methods; mi++) {
            TMethod m = t->methods[mi];
            m.decl = cls;      /* B3: trait 方法归使用类（private 对使用类可见） */
            const char *mlc = tstr_val(m.name);
            /* `as` adaptations first: a renamed method escapes insteadof
             * exclusion (PHP semantics) */
            bool renamed = false;
            for (int k = 0; mods && k < mods->n; k++) {
                AMod *d = &mods->items[k];
                if (d->is_insteadof) continue;
                bool hit = d->trait
                    ? tstr_case_eq(tstr_val(d->trait), tstr_val(t->name))
                    : true;
                if (!hit || !tstr_case_eq(tstr_val(d->method), mlc)) continue;
                if (d->new_name) {
                    m.name = d->new_name;
                    renamed = true;
                }
                if (d->new_vis >= 0) m.visibility = (uint8_t)d->new_vis;
            }
            /* insteadof: another trait wins the ORIGINAL name (renames skip) */
            if (!renamed) {
                bool excluded = false;
                for (int k = 0; mods && k < mods->n; k++) {
                    AMod *d = &mods->items[k];
                    if (!d->is_insteadof) continue;
                    if (!tstr_case_eq(tstr_val(d->method), mlc)) continue;
                    if (!d->trait || tstr_case_eq(tstr_val(d->trait),
                                                  tstr_val(t->name)))
                        continue;   /* rule names the winner, not us */
                    for (int f = 0; f < d->n_from; f++)
                        if (tstr_case_eq(tstr_val(d->from[f]), tstr_val(t->name)))
                            excluded = true;
                }
                if (excluded) continue;
            }
            /* conflict resolution: earlier trait wins unless an insteadof
             * rule in the mods names this trait as the winner */
            bool replaced = false;
            for (int i = 0; i < cls->n_methods; i++) {
                if (!tstr_case_eq(tstr_val(cls->methods[i].name),
                                  tstr_val(m.name))) continue;
                bool rule_allows = false;
                for (int k = 0; mods && k < mods->n; k++) {
                    AMod *d = &mods->items[k];
                    if (!d->is_insteadof) continue;
                    if (!tstr_case_eq(tstr_val(d->method), tstr_val(m.name)))
                        continue;
                    if (d->trait && tstr_case_eq(tstr_val(d->trait),
                                                 tstr_val(t->name)))
                        rule_allows = true;
                }
                if (!rule_allows)
                    tphp_fatal_src("class.c", 0,
                        "Trait method collision on %s (use insteadof)",
                        tstr_val(m.name));
                cls->methods[i] = m;
                replaced = true;
                break;
            }
            if (!replaced) add_method(cls, m);
        }
        for (int pi = 0; pi < t->n_props; pi++) {
            TProp p = t->props[pi];
            p.decl = cls;   /* trait 属性归使用类（readonly 写入作用域同） */
            add_prop(cls, p);
        }
        for (int pi = 0; pi < t->n_sprops; pi++) {
            TProp p = t->sprops[pi];
            p.decl = cls;   /* trait 静态属性 = 类自有（PHP：每个使用类各一份） */
            add_sprop(cls, p);
        }
        /* P2-B: trait constants. If the class (or an earlier parent/trait)
         * already defines the name, the existing entry wins — PHP 8.3
         * fatals on a real collision, approximated here (see TODO P3). */
        for (int ci = 0; ci < t->n_consts; ci++) {
            bool exists = false;
            for (int i = 0; i < cls->n_consts; i++)
                if (tstr_case_eq(tstr_val(cls->cnames[i]),
                                 tstr_val(t->cnames[ci]))) { exists = true; break; }
            if (exists) continue;
            cls->cnames = tm_realloc(cls->cnames,
                sizeof(TStr *) * (size_t)(cls->n_consts + 1));
            cls->cvals = tm_realloc(cls->cvals,
                sizeof(TVal) * (size_t)(cls->n_consts + 1));
            cls->cthunks = tm_realloc(cls->cthunks,
                sizeof(uint16_t) * (size_t)(cls->n_consts + 1));
            cls->cnames[cls->n_consts] = t->cnames[ci];
            tval_addref(t->cvals[ci]);
            cls->cvals[cls->n_consts] = t->cvals[ci];
            /* P2-B: 惰性常量表达式的 thunk 索引随导入共享（prog 级只读） */
            cls->cthunks[cls->n_consts] = t->cthunks ? t->cthunks[ci] : 0;
            cls->n_consts++;
        }
    }
    /* adaptation bookkeeping is compile-lifetime; drop the references */
    cls->n_trait_names = 0;
    cls->trait_mods = NULL;
}

void tphp_class_ensure_flat(TClass *cls) {
    if (cls->flattened) return;
    cls->flattened = true;   /* set early to break cycles on broken hierarchies */

    /* parent first */
    TClass *parent = NULL;
    if (cls->parent_name) {
        char lc[256];
        lc_copy(lc, tstr_val(cls->parent_name), sizeof lc);
        parent = class_resolve_lc2(lc, tstr_val(cls->parent_name));
        if (!parent)
            tphp_fatal_src("class.c", 0, "Class \"%s\" not found",
                           tstr_val(cls->parent_name));
        if (parent->kind == CK_TRAIT)
            tphp_fatal_src("class.c", 0, "Cannot extend a trait");
        if (parent->kind == CK_INTERFACE)
            tphp_fatal_src("class.c", 0, "Class cannot extend an interface; use implements");
        if ((parent->flags & CLS_FINAL))
            tphp_fatal_src("class.c", 0, "Class %s may not inherit from final class",
                           tstr_val(parent->name));
        cls->parent = parent;
        cls->parent_name = NULL;
        tphp_class_ensure_flat(parent);
    }
    /* traits bind after the parent (PHP 8.5 order) — trait_names are resolved
     * in the trait-binding pass (see bind_traits below) */

    /* stash the own (pre-flat) member tables compile.c built */
    TMethod *om = cls->methods; int nom = cls->n_methods;
    TProp *op = cls->props; int nop = cls->n_props;
    TProp *osp = cls->sprops; int nosp = cls->n_sprops;
    TVal *ospv = cls->sprop_vals;
    cls->methods = NULL; cls->n_methods = 0;
    cls->props = NULL; cls->n_props = 0;
    cls->sprops = NULL; cls->n_sprops = 0;
    cls->sprop_vals = NULL;

    /* start from copies of the parent's flat tables */
    if (parent) {
        cls->methods = tm_malloc(sizeof(TMethod) * (size_t)(parent->n_methods ? parent->n_methods : 1));
        memcpy(cls->methods, parent->methods, sizeof(TMethod) * (size_t)parent->n_methods);
        cls->n_methods = parent->n_methods;
        cls->props = tm_malloc(sizeof(TProp) * (size_t)(parent->n_props ? parent->n_props : 1));
        memcpy(cls->props, parent->props, sizeof(TProp) * (size_t)parent->n_props);
        cls->n_props = parent->n_props;
        cls->sprops = tm_malloc(sizeof(TProp) * (size_t)(parent->n_sprops ? parent->n_sprops : 1));
        memcpy(cls->sprops, parent->sprops, sizeof(TProp) * (size_t)parent->n_sprops);
        cls->n_sprops = parent->n_sprops;
        cls->sprop_vals = tm_malloc(sizeof(TVal) * (size_t)(parent->n_sprops ? parent->n_sprops : 1));
        memcpy(cls->sprop_vals, parent->sprop_vals, sizeof(TVal) * (size_t)parent->n_sprops);
        for (int i = 0; i < parent->n_iface_names; i++)
            add_iface_name(cls, parent->iface_names[i]);
    }

    /* merge own members (child overrides parent) */
    for (int i = 0; i < nom; i++) add_method(cls, om[i]);
    for (int i = 0; i < nop; i++) add_prop(cls, op[i]);
    for (int i = 0; i < nosp; i++) add_sprop(cls, osp[i]);
    if (om) tm_free(om);
    if (op) tm_free(op);
    if (osp) tm_free(osp);
    if (ospv) tm_free(ospv);

    /* traits bind after the parent and after own members (own class
     * methods take precedence over trait methods, per PHP) */
    bind_traits(cls);

    /* interfaces */
    for (int i = 0; i < cls->n_iface_names; i++) {
        char lc[256];
        lc_copy(lc, tstr_val(cls->iface_names[i]), sizeof lc);
        TClass *ifc = class_resolve_lc2(lc, tstr_val(cls->iface_names[i]));
        if (!ifc || ifc->kind != CK_INTERFACE)
            tphp_fatal_src("class.c", 0, "Interface \"%s\" not found",
                           tstr_val(cls->iface_names[i]));
        bool dup = false;
        for (int k = 0; k < cls->n_ifaces; k++)
            if (cls->ifaces[k] == ifc) { dup = true; break; }
        if (!dup) {
            cls->ifaces = tm_realloc(cls->ifaces,
                                     sizeof(TClass *) * (size_t)(cls->n_ifaces + 1));
            cls->ifaces[cls->n_ifaces++] = ifc;
            for (int k = 0; k < ifc->n_iface_names; k++)
                add_iface_name(cls, ifc->iface_names[k]);
        }
    }

    /* interface constant inheritance: an interface carries the constants
     * of every interface it extends (own declarations win) */
    if (cls->kind == CK_INTERFACE) {
        for (int i = 0; i < cls->n_ifaces; i++) {
            TClass *ifc = cls->ifaces[i];
            tphp_class_ensure_flat(ifc);
            for (int k = 0; k < ifc->n_consts; k++) {
                bool have = false;
                for (int j = 0; j < cls->n_consts; j++)
                    if (tstr_case_eq(tstr_val(cls->cnames[j]),
                                     tstr_val(ifc->cnames[k]))) { have = true; break; }
                if (have) continue;
                cls->cnames = tm_realloc(cls->cnames,
                    sizeof(TStr *) * (size_t)(cls->n_consts + 1));
                cls->cvals = tm_realloc(cls->cvals,
                    sizeof(TVal) * (size_t)(cls->n_consts + 1));
                cls->cthunks = tm_realloc(cls->cthunks,
                    sizeof(uint16_t) * (size_t)(cls->n_consts + 1));
                cls->cnames[cls->n_consts] = ifc->cnames[k];
                TVal v = ifc->cvals[k];
                tval_addref(v);
                cls->cvals[cls->n_consts] = v;
                /* P2-B: 惰性常量 thunk 索引随接口继承共享 */
                cls->cthunks[cls->n_consts] = ifc->cthunks ? ifc->cthunks[k] : 0;
                cls->n_consts++;
            }
        }
    }

    /* magic-method quick links */
    cls->m_ctor = cls->m_dtor = cls->m_get = cls->m_set = cls->m_isset =
        cls->m_unset = cls->m_call = cls->m_callstatic = cls->m_tostring =
        cls->m_invoke = cls->m_clone = cls->m_debuginfo = cls->m_clonestate = -1;
    for (int i = 0; i < cls->n_methods; i++) {
        char lc[128];
        lc_copy(lc, tstr_val(cls->methods[i].name), sizeof lc);
        int16_t code = magic_index(lc);
        if (code <= -3) {
            int16_t *slot = quick_slot(cls, code);
            if (slot) *slot = (int16_t)i;
        }
    }

    /* interface contract: a concrete class may not carry abstract methods
     * (PHP fatals at declaration time). native 方法（内部类）fn 为 NULL 但
     * 不是抽象——exclass.c 的内置异常类靠它注册 ctor/getter（P1-2）。 */
    if (cls->kind == CK_CLASS && !(cls->flags & CLS_ABSTRACT)) {
        for (int i = 0; i < cls->n_methods; i++) {
            if (cls->methods[i].is_abstract ||
                (!cls->methods[i].fn && !cls->methods[i].native))
                tphp_fatal_src("class.c", 0,
                    "Class %s contains abstract method %s::%s() and must "
                    "therefore be declared abstract",
                    tstr_val(cls->name), tstr_val(cls->name),
                    tstr_val(cls->methods[i].name));
        }
    }
}

/* ---- lookups over the flat table ---- */
TMethod *tphp_class_find_method(TClass *cls, const char *name_lc) {
    tphp_class_ensure_flat(cls);
    for (int i = 0; i < cls->n_methods; i++)
        if (tstr_case_eq(tstr_val(cls->methods[i].name), name_lc))
            return &cls->methods[i];
    return NULL;
}

int tphp_class_find_prop(TClass *cls, const char *name_lc) {
    tphp_class_ensure_flat(cls);
    for (int i = 0; i < cls->n_props; i++)
        if (tstr_case_eq(tstr_val(cls->props[i].name), name_lc))
            return cls->props[i].slot;
    return -1;
}

TProp *tphp_class_find_sprop(TClass *cls, const char *name_lc) {
    tphp_class_ensure_flat(cls);
    for (int i = 0; i < cls->n_sprops; i++)
        if (tstr_case_eq(tstr_val(cls->sprops[i].name), name_lc))
            return &cls->sprops[i];
    return NULL;
}

bool tphp_class_has_const(TClass *cls, const char *name_lc, TVal *out,
                          TClass **owner, int *idx) {
    for (TClass *c = cls; c; c = c->parent) {
        for (int i = 0; i < c->n_consts; i++) {
            if (tstr_case_eq(tstr_val(c->cnames[i]), name_lc)) {
                if (out) *out = c->cvals[i];
                if (owner) *owner = c;
                if (idx) *idx = i;
                return true;
            }
        }
    }
    return false;
}

/* ---- hierarchy ---- */
bool tphp_class_isa(TClass *cls, TClass *ancestor) {
    tphp_class_ensure_flat(cls);
    if (ancestor->kind == CK_INTERFACE) {
        for (int i = 0; i < cls->n_ifaces; i++)
            if (cls->ifaces[i] == ancestor) return true;
        return false;
    }
    for (TClass *c = cls; c; c = c->parent) {
        tphp_class_ensure_flat(c);
        if (c == ancestor) return true;
    }
    return false;
}

bool tphp_class_isa_name(TClass *cls, const char *fqcn_lc) {
    TClass *b = class_resolve_lc(fqcn_lc);
    return b ? tphp_class_isa(cls, b) : false;
}

bool tphp_class_inst_op(TClass *a, TClass *b) {
    return tphp_class_isa(a, b);
}

bool tphp_vis_ok(uint8_t vis, TClass *owner, TClass *from) {
    if (vis == CVIS_PUBLIC) return true;
    if (!from) return false;
    if (vis == CVIS_PRIVATE) {
        /* B3 对拍（PHP 8.5 实测）：private 仅声明类作用域精确可见——
         * 父类私有对子类 scope 不可见（$this->parentPrivate / parent::f()
         * 均不可达），原 parent 链遍历与 PHP 冲突 */
        return from == owner;
    }
    /* protected: owner must be in from's hierarchy either way */
    for (TClass *c = from; c; c = c->parent)
        if (c == owner) return true;
    for (TClass *c = owner; c; c = c->parent)
        if (c == from) return true;
    return false;
}

/* ---- object allocation ---- */
TObj *tphp_obj_alloc(TClass *cls) {
    tphp_class_ensure_flat(cls);
    TObj *o = tm_malloc(sizeof(TObj));
    o->h.rc = 1; o->h.color = GC_COLOR_UNMARKED; o->h.kind = GC_KIND_OBJ;
    o->h.flags = 0;
    gc_register(&o->h);
    o->cls = cls;
    extern _Thread_local uint32_t tphp_obj_next_id;
    o->oid = tphp_obj_next_id++;
    int np = cls->n_props;
    o->props = tm_malloc(sizeof(TVal) * (size_t)(np ? np : 1));
    for (int i = 0; i < np; i++) {
        TProp *p = &cls->props[i];
        if (p->def_thunk) {
            /* constant-expression default: evaluated via the VM thunk
             * machinery on the interpreter's current VM */
            extern _Thread_local void *tphp_vm_current;
            extern TVal tphp_call_thunk(struct VM *vm, uint16_t fn_idx);
            o->props[i] = tphp_call_thunk(tphp_vm_current, p->def_thunk);
        } else if (p->defval.t == T_UNDEF) {
            o->props[i] = (TVal){ .u.i = 0, .t = T_UNDEF };   /* unset */
        } else {
            o->props[i] = p->defval;
            tval_addref(o->props[i]);
        }
    }
    return o;
}

TObj *tphp_enum_case_obj(TClass *cls, int case_idx) {
    /* singleton cached per-thread (M5): the shared TClass must not be
     * mutated at runtime, so case objects live in the side table */
    ClsRt *rt = cls_rt_get(cls);
    TObj *o = rt->case_objs[case_idx];
    if (!o) {
        o = tphp_obj_alloc(cls);
        o->h.rc++;                        /* the side table holds one ref */
        rt->case_objs[case_idx] = o;
    }
    o->h.rc++;           /* the caller takes ownership of one ref */
    return o;
}

/* reverse lookup: which case does this singleton object belong to?
 * (-1 when o is not an enum case instance) */
int tphp_enum_case_find(TClass *cls, TObj *o) {
    ClsRt *rt = cls_rt_get(cls);
    for (int i = 0; i < cls->n_cases; i++)
        if (rt->case_objs[i] == o) return i;
    return -1;
}

/* ---- freeze: eager flatten of every master class (M5) ----
 * After this, ensure_flat calls at runtime are no-ops (flattened=true),
 * so the shared TClass structures are never mutated while worker threads
 * read them. */
void tphp_class_flatten_all(void) {
    for (uint32_t i = 0; i < cls_cap_m; i++)
        if (cls_tab_master[i].cls)
            tphp_class_ensure_flat(cls_tab_master[i].cls);
}

/* ---- worker-thread class table (M5) ----
 * Deep-copy every master-table class into the calling thread's registry:
 * structure fields are frozen (flatten must have completed), parent/iface
 * links are re-pointed into the cloned graph, per-thread value templates
 * (defval/cvals/sprop_vals/case backing) are deep-copied. */
typedef struct { TClass *old; TClass *nw; } ClsClonePair;

/* B3: decl 重指向（原指针指向 master 图；worker 的 from 类在克隆图中，
 * 不重指向则 private/protected 判定跨图永假） */
static TClass *decl_remap(ClsClonePair *pairs, uint32_t n, TClass *d) {
    if (!d) return NULL;
    for (uint32_t j = 0; j < n; j++)
        if (pairs[j].old == d) return pairs[j].nw;
    return d;
}

void tphp_class_tab_clone_thread(void) {
    /* collect master entries */
    ClsClonePair *pairs = tm_malloc(sizeof(ClsClonePair) *
                                    (size_t)(cls_n_m ? cls_n_m : 1));
    uint32_t n = 0;
    for (uint32_t i = 0; i < cls_cap_m; i++) {
        TClass *c = cls_tab_master[i].cls;
        if (!c) continue;
        TClass *nc = tm_malloc(sizeof *nc);
        *nc = *c;                         /* frozen structure fields */
        nc->parent = NULL;                /* re-pointed below */
        nc->ifaces = NULL; nc->n_ifaces = 0;
        nc->sprop_vals = NULL;            /* re-seeded below (thread-local) */
        pairs[n].old = c; pairs[n].nw = nc; n++;
        /* register into this thread's table (key_lc re-derived) */
        if (!tphp_class_register(nc))
            tphp_fatal_src("class.c", 0, "duplicate class in thread clone");
    }
    /* deep-copy per-thread value templates (arrays that runtime writes
     * must be thread-local: props/sprops defvals, cvals, cases backing) */
    for (uint32_t k = 0; k < n; k++) {
        TClass *c = pairs[k].old, *nc = pairs[k].nw;
        if (nc->n_props) {
            TProp *np = tm_malloc(sizeof(TProp) * (size_t)nc->n_props);
            memcpy(np, c->props, sizeof(TProp) * (size_t)nc->n_props);
            nc->props = np;
            for (int i = 0; i < nc->n_props; i++)
                nc->props[i].defval = tval_clone_deep(np[i].defval);
        }
        if (nc->n_methods) {
            /* B3: 原与 master 共享——decl 重指向需要独立副本 */
            TMethod *nmm = tm_malloc(sizeof(TMethod) * (size_t)nc->n_methods);
            memcpy(nmm, c->methods, sizeof(TMethod) * (size_t)nc->n_methods);
            nc->methods = nmm;
        }
        if (nc->n_sprops) {
            TProp *nsp = tm_malloc(sizeof(TProp) * (size_t)nc->n_sprops);
            memcpy(nsp, c->sprops, sizeof(TProp) * (size_t)nc->n_sprops);
            nc->sprops = nsp;
            for (int i = 0; i < nc->n_sprops; i++)
                nc->sprops[i].defval = tval_clone_deep(nsp[i].defval);
            /* static-prop template: deep copy of the original seed (the
             * side table deep-copies from it on first access) */
            nc->sprop_vals = tm_malloc(sizeof(TVal) * (size_t)nc->n_sprops);
            for (int i = 0; i < nc->n_sprops; i++)
                nc->sprop_vals[i] = tval_clone_deep(c->sprop_vals[i]);
        }
        if (nc->n_consts) {
            TVal *ncv = tm_malloc(sizeof(TVal) * (size_t)nc->n_consts);
            memcpy(ncv, c->cvals, sizeof(TVal) * (size_t)nc->n_consts);
            nc->cvals = ncv;              /* cnames stay shared (interned) */
            for (int i = 0; i < nc->n_consts; i++)
                nc->cvals[i] = tval_clone_deep(ncv[i]);
            /* P2-B: 惰性常量 thunk 索引是 prog 级（funcs 冻结只读），直接拷 */
            if (c->cthunks) {
                nc->cthunks = tm_malloc(sizeof(uint16_t) * (size_t)nc->n_consts);
                memcpy(nc->cthunks, c->cthunks,
                       sizeof(uint16_t) * (size_t)nc->n_consts);
            }
        }
        if (nc->n_cases) {
            TEnumCase *nec = tm_malloc(sizeof(TEnumCase) * (size_t)nc->n_cases);
            memcpy(nec, c->cases, sizeof(TEnumCase) * (size_t)nc->n_cases);
            nc->cases = nec;
            for (int i = 0; i < nc->n_cases; i++) {
                nec[i].obj = NULL;
                nec[i].backing = tval_clone_deep(nec[i].backing);
            }
        }
    }
    /* re-point parent/iface links into the cloned graph */
    for (uint32_t k = 0; k < n; k++) {
        TClass *c = pairs[k].old, *nc = pairs[k].nw;
        if (c->parent) {
            for (uint32_t j = 0; j < n; j++)
                if (pairs[j].old == c->parent) { nc->parent = pairs[j].nw; break; }
        }
        if (c->n_ifaces) {
            nc->ifaces = tm_malloc(sizeof(TClass *) * (size_t)c->n_ifaces);
            nc->n_ifaces = c->n_ifaces;
            for (int i = 0; i < c->n_ifaces; i++) {
                nc->ifaces[i] = NULL;
                for (uint32_t j = 0; j < n; j++)
                    if (pairs[j].old == c->ifaces[i]) { nc->ifaces[i] = pairs[j].nw; break; }
            }
        }
        /* B3: 声明类指针重指向克隆图（props/sprops/methods 的 decl） */
        for (int i = 0; i < nc->n_props; i++)
            nc->props[i].decl = decl_remap(pairs, n, nc->props[i].decl);
        for (int i = 0; i < nc->n_sprops; i++)
            nc->sprops[i].decl = decl_remap(pairs, n, nc->sprops[i].decl);
        for (int i = 0; i < nc->n_methods; i++)
            nc->methods[i].decl = decl_remap(pairs, n, nc->methods[i].decl);
    }
    tm_free(pairs);
}
