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

/* 运行期注册（内部类 Async\Coroutine 等）：只进本线程表 */
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
    TObj **case_objs;      /* n_cases slots (enum singletons) */
    struct ClsRt *next;
} ClsRt;

static _Thread_local ClsRt *g_cls_rt;

static ClsRt *cls_rt_get(TClass *cls) {
    for (ClsRt *r = g_cls_rt; r; r = r->next)
        if (r->cls == cls) return r;
    ClsRt *r = tm_malloc(sizeof *r);
    r->cls = cls;
    r->sprop_vals = tm_malloc(sizeof(TVal) *
                              (size_t)(cls->n_sprops ? cls->n_sprops : 1));
    /* seed from the frozen template (deep copy: per-thread heap values) */
    for (int i = 0; i < cls->n_sprops; i++)
        r->sprop_vals[i] = tval_clone_deep(cls->sprop_vals[i]);
    r->case_objs = tm_malloc(sizeof(TObj *) *
                             (size_t)(cls->n_cases ? cls->n_cases : 1));
    for (int i = 0; i < cls->n_cases; i++) r->case_objs[i] = NULL;
    r->next = g_cls_rt;
    g_cls_rt = r;
    return r;
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
 * miss (runtime only — during load there is no VM to run PHP code in) */
TClass *class_resolve_lc(const char *fqcn_lc) {
    TClass *c = tphp_class_lookup(fqcn_lc);
    if (c) return c;
    if (autoload_fn.t != T_NULL && tphp_vm_current) {
        TStr *req = tstr_intern(fqcn_lc, strlen(fqcn_lc));
        tval_addref(TV_STR(req));
        tphp_vm_run_autoloader(autoload_fn, req);
        return tphp_class_lookup(fqcn_lc);
    }
    return NULL;
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
            p.slot = cls->sprops[i].slot;
            cls->sprops[i] = p;         /* value slot keeps the parent entry */
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
        TClass *t = class_resolve_lc(tlc);
        if (!t || t->kind != CK_TRAIT)
            tphp_fatal_src("class.c", 0, "Trait '%s' not found",
                           tstr_val(cls->trait_names[ti]));
        for (int mi = 0; mi < t->n_methods; mi++) {
            TMethod m = t->methods[mi];
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
        for (int pi = 0; pi < t->n_props; pi++)
            add_prop(cls, t->props[pi]);
        for (int pi = 0; pi < t->n_sprops; pi++)
            add_sprop(cls, t->sprops[pi]);
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
        parent = class_resolve_lc(lc);
        if (!parent)
            tphp_fatal_src("class.c", 0, "Class '%s' not found",
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
        TClass *ifc = class_resolve_lc(lc);
        if (!ifc || ifc->kind != CK_INTERFACE)
            tphp_fatal_src("class.c", 0, "Interface '%s' not found",
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
                cls->cnames[cls->n_consts] = ifc->cnames[k];
                TVal v = ifc->cvals[k];
                tval_addref(v);
                cls->cvals[cls->n_consts++] = v;
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
     * (PHP fatals at declaration time) */
    if (cls->kind == CK_CLASS && !(cls->flags & CLS_ABSTRACT)) {
        for (int i = 0; i < cls->n_methods; i++) {
            if (cls->methods[i].is_abstract || !cls->methods[i].fn)
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

bool tphp_class_has_const(TClass *cls, const char *name_lc, TVal *out) {
    for (TClass *c = cls; c; c = c->parent) {
        for (int i = 0; i < c->n_consts; i++) {
            if (tstr_case_eq(tstr_val(c->cnames[i]), name_lc)) {
                if (out) *out = c->cvals[i];
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
        for (TClass *c = from; c; c = c->parent)
            if (c == owner) return true;
        return false;
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
    }
    tm_free(pairs);
}
