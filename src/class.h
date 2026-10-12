/*
 * TinyPHP.rt - class.c API: class registry, flattening, member lookup
 *
 * TClass is program-lifetime. Classes are declared in three steps:
 *   1. compile.c builds the TClass with own members (methods[], props[],
 *      cnames[]) and unresolved parent/iface/trait names
 *   2. tphp_class_register_compile() inserts it into the registry (and the
 *      process-level master table, the clone source for worker threads);
 *      tphp_class_register() registers into the calling thread only
 *   3. flattening (inheriting parent members, binding traits) happens
 *      lazily on first use via tphp_class_ensure_flat() — or eagerly for
 *      every class via tphp_class_flatten_all() (prog freeze, M5)
 */
#ifndef TPHP_CLASS_H
#define TPHP_CLASS_H

#include "value.h"
#include "opcode.h"

/* ---- registry ----
 * name must be the interned FQCN (lowercased? no: PHP class names are
 * case-insensitive; we store the declared spelling and match
 * case-insensitively via tstr_case_eq). Returns false on duplicate. */
bool tphp_class_register_compile(TClass *cls);
bool tphp_class_register(TClass *cls);
TClass *tphp_class_lookup(const char *fqcn_lc);
bool tphp_class_exists(const char *fqcn_lc);

/* worker-thread setup (M5): deep-copy every class of the process-level
 * master table into the calling thread's registry. Runtime mutable state
 * (static property values, enum singletons) is per-thread afterwards. */
void tphp_class_tab_clone_thread(void);

/* eager flatten of every master-table class (freeze step; after this the
 * lazy ensure_flat calls in the VM are no-ops) */
void tphp_class_flatten_all(void);

/* per-thread static-property storage (side table keyed by TClass*):
 * returns the thread-local value slots (n_sprops+1), seeded from the
 * class's frozen template on first access */
TVal *tphp_class_sprop_vals(TClass *cls);

/* lazy flattening: inherit parent members, bind traits, build the
 * magic-method quick links. Recurses to the parent first. */
void tphp_class_ensure_flat(TClass *cls);

/* member lookup along the flattened table (O(n_methods), fine for now) */
TMethod *tphp_class_find_method(TClass *cls, const char *name_lc);
int  tphp_class_find_prop(TClass *cls, const char *name_lc);   /* slot or -1 */
TProp *tphp_class_find_sprop(TClass *cls, const char *name_lc);
bool tphp_class_has_const(TClass *cls, const char *name_lc, TVal *out);

/* hierarchy */
bool tphp_class_isa(TClass *cls, TClass *ancestor);   /* includes self, ifaces */
bool tphp_class_isa_name(TClass *cls, const char *fqcn_lc);
bool tphp_class_inst_op(TClass *a, TClass *b);       /* allow_substitutions of b */

/* enum case singleton: lazy per-thread creation + reverse lookup */
TObj *tphp_enum_case_obj(TClass *cls, int case_idx);
int tphp_enum_case_find(TClass *cls, TObj *o);       /* -1 = not a case obj */

/* __cloneState protocol (M5-b; vm.c):
 *   clone_state runs __cloneState(): array (+1 result); false = no method
 *   restore_clone_state prefers __restoreCloneState(array), else fills
 *   same-named property slots from the state array's string keys */
bool tphp_vm_clone_state(struct VM *vm, TObj *o, TVal *out);
void tphp_vm_restore_clone_state(struct VM *vm, TObj *o, TVal state);

/* visibility: is `vis` on `owner` accessible from scope class `from`?
 * from == NULL means global scope (public only). */
bool tphp_vis_ok(uint8_t vis, TClass *owner, TClass *from);

/* ---- object allocation (does NOT run the constructor) ---- */
TObj *tphp_obj_alloc(TClass *cls);

/* case-insensitive compare for identifiers (ASCII, PHP semantics) */
bool tstr_case_eq(const char *a, const char *b);

/* autoload (spl_autoload_register): fn is a PHP-level callable — a TClosure
 * value or a function-name string; run by the VM on class-lookup miss */
void tphp_class_set_autoload_val(TVal fn);
void tphp_vm_run_autoloader(TVal fn, TStr *name);   /* vm.c */

/* runtime resolve (lookup + one autoloader run on miss); class.c */
TClass *class_resolve_lc(const char *fqcn_lc);

/* global class table reset (not needed: program lifetime) */

#endif /* TPHP_CLASS_H */
