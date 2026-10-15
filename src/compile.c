/*
 * TinyPHP.rt - a small PHP runtime
 * compile.c - AST -> opcode (load phase)
 *
 * Slot model: a frame has [CV 0..n_locals) then [TMP ..+n_tmps). During
 * compilation TMP slots use virtual numbers; ctx_finish() relocates every
 * OPD_TMP operand by adding n_locals. `global` aliases never occupy
 * storage slots in function frames: reads/writes compile to *_GLOBAL
 * opcodes that operate on the main frame (the global scope) by name.
 * The main frame IS the global scope: all its CVs are globals, so the
 * compiler pre-creates a slot for every name that any function declares
 * `global` (prescan) and compiles main-frame variables as plain CVs.
 *
 * Jump target convention: OP_JMP/JMPZ/JMPNZ/JMP_NULL keep the condition
 * in o1 and the target in ar; patch_jump() writes ar.
 *
 * Writable dim chains ($a[0][1] = v) use a write-back list: the base of
 * every level is a slot the mutating handler may rewrite (COW separation /
 * auto-vivification); after the mutation, each level is stored back into
 * its parent (innermost first, which is the natural list order).
 */
#include "opcode.h"
#include "ast.h"
#include "lex.h"
#include "class.h"
#include "bundle.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tphp_ex_class_index(const char *name);

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

/* ============ namespace state (load phase) ============
 * Single-threaded compile: file-level state tracks the current namespace
 * and the use-table. Names resolve per PHP rules:
 *   - leading '\'  -> absolute (used verbatim)
 *   - use-imported -> replaced by the imported FQ name
 *   - qualified    -> current NS prefix, no fallback
 *   - unqualified  -> classes: NS prefix, no fallback; functions/constants:
 *                     NS prefix with a runtime fallback to the bare name */
typedef struct { TStr *alias, *fq; } UseEnt;
typedef struct {
    UseEnt *cls;    int n_cls,  cap_cls;
    UseEnt *func;   int n_func, cap_func;
    UseEnt *konst;  int n_konst, cap_konst;
} UseTab;

static TStr  *s_cur_ns;      /* NULL or "" = global namespace */
static UseTab s_use;

static void apply_use_list(AUseList *ul);   /* fwd: defined in pass section */

static void ns_reset(void) {
    s_cur_ns = NULL;
    memset(&s_use, 0, sizeof s_use);
}

static void ns_enter(TStr *name) { s_cur_ns = name; }

static TStr *ns_concat(TStr *prefix, TStr *name) {
    const char *a = tstr_val(prefix), *b = tstr_val(name);
    size_t la = strlen(a), lb = strlen(b);
    char *tmp = tm_malloc(la + lb + 2);
    memcpy(tmp, a, la);
    tmp[la] = '\\';
    memcpy(tmp + la + 1, b, lb + 1);
    TStr *out = tstr_intern(tmp, la + 1 + lb);
    tm_free(tmp);
    return out;
}

/* PHP use-import matching: the leading segment of the referenced name
 * (case-insensitively) matches an alias */
static TStr *use_match(UseEnt *tab, int n, TStr *name) {
    const char *s = tstr_val(name);
    size_t seg = strcspn(s, "\\");
    for (int i = 0; i < n; i++) {
        const char *al = tstr_val(tab[i].alias);
        if (strlen(al) == seg) {
            size_t k = 0;
            for (; k < seg; k++) {
                char c1 = s[k], c2 = al[k];
                if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
                if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
                if (c1 != c2) break;
            }
            if (k == seg) return tab[i].fq;
        }
    }
    return NULL;
}

/* class/interface/trait/enum name -> FQ (classes never fall back) */
static TStr *resolve_class_name(TStr *name) {
    const char *s = tstr_val(name);
    if (s[0] == '\\') return tstr_intern(s + 1, strlen(s) - 1);
    TStr *hit = use_match(s_use.cls, s_use.n_cls, name);
    if (hit) return hit;
    if (!s_cur_ns || tstr_len(s_cur_ns) == 0) return name;
    return ns_concat(s_cur_ns, name);
}

/* function name -> FQ; *fallback = try the bare global name at runtime */
static TStr *resolve_func_name(TStr *name, bool *fallback) {
    *fallback = false;
    const char *s = tstr_val(name);
    if (s[0] == '\\') return tstr_intern(s + 1, strlen(s) - 1);
    TStr *hit = use_match(s_use.func, s_use.n_func, name);
    if (hit) return hit;
    if (strchr(s, '\\')) {                       /* qualified: no fallback */
        if (!s_cur_ns || tstr_len(s_cur_ns) == 0) return name;
        return ns_concat(s_cur_ns, name);
    }
    if (!s_cur_ns || tstr_len(s_cur_ns) == 0) return name;
    *fallback = true;                            /* unqualified in a NS */
    return ns_concat(s_cur_ns, name);
}

/* declaration name -> FQ (function/const declarations): absolute \ strips,
 * otherwise the current NS prefix; use imports do NOT apply to declarations */
static TStr *resolve_decl_name(TStr *name) {
    const char *s = tstr_val(name);
    if (s[0] == '\\') return tstr_intern(s + 1, strlen(s) - 1);
    if (!s_cur_ns || tstr_len(s_cur_ns) == 0) return name;
    return ns_concat(s_cur_ns, name);
}

/* constant name -> FQ; fallback semantics equal functions */
static TStr *resolve_const_name(TStr *name, bool *fallback) {
    *fallback = false;
    const char *s = tstr_val(name);
    if (s[0] == '\\') return tstr_intern(s + 1, strlen(s) - 1);
    TStr *hit = use_match(s_use.konst, s_use.n_konst, name);
    if (hit) return hit;
    if (strchr(s, '\\')) {
        if (!s_cur_ns || tstr_len(s_cur_ns) == 0) return name;
        return ns_concat(s_cur_ns, name);
    }
    if (!s_cur_ns || tstr_len(s_cur_ns) == 0) return name;
    *fallback = true;
    return ns_concat(s_cur_ns, name);
}

enum { CAST_INT = 1, CAST_BOOL, CAST_FLOAT, CAST_STRING, CAST_ARRAY };

static void compile_class_decl(Program *p, AClassDecl *cd, const char *file);

typedef struct { uint8_t kind; int slot; } Loc;
#define LOC_NONE    ((Loc){ .kind = OPD_UNUSED, .slot = 0 })
#define LOC_CV(s)   ((Loc){ .kind = OPD_CV,   .slot = (s) })
#define LOC_TMP(s)  ((Loc){ .kind = OPD_TMP,  .slot = (s) })
#define LOC_CONST(i)((Loc){ .kind = OPD_CONST,.slot = (int)(i) })

typedef struct LoopPatches {
    bool is_switch;
    int *breaks; int n_breaks, cap_breaks;
    int *conts;  int n_conts, cap_conts;
    struct LoopPatches *prev;
} LoopPatches;

/* compile-time function signatures (named-argument reordering) */
typedef struct FnSig {
    TStr *name;
    TStr **params; int n_params;
} FnSig;

typedef struct Ctx {
    const char *file;
    Program *prog;
    TFunc *fn;
    bool is_main;
    /* variable table: name -> CV slot, or global alias (no slot) */
    struct { TStr *name; int slot; bool is_global; } *vars;
    int n_vars, cap_vars;
    int n_tmps;
    LoopPatches *loops;
    /* every name declared `global` anywhere in the program (shared) */
    TStr **global_names; int n_global_names;
    /* top-level signatures (shared; named-argument reorder) */
    FnSig *sigs; int n_sigs;
    /* goto/label fixups (function-scoped) */
    struct { TStr *label; int jmp_pc; } *gotos; int n_gotos;
    struct { TStr *label; int pc; } *labels; int n_labels;
    /* finally-through-return trampolines (innermost first) */
    struct FinCtx *fin;
} Ctx;

/* a `return` inside a try body jumps to a per-return copy of the finally
 * block; after the copy, RETURN restores the stashed value from val_tmp */
typedef struct FinCtx {
    struct { int jmp_pc; int val_tmp; } *pend; int n_pend, cap_pend;
    bool no_return;    /* compiling a catch body under this finally */
    struct FinCtx *prev;
} FinCtx;

/* ---- emit ---- */
static int emit(Ctx *c, Opcode op, Loc o1, Loc o2, Loc ar, int line) {
    TFunc *fn = c->fn;
    fn->ops = tm_realloc(fn->ops, sizeof(TOp) * (size_t)(fn->n_ops + 1));
    TOp *o = &fn->ops[fn->n_ops];
    memset(o, 0, sizeof *o);
    o->handler = (void *)tphp_op_handlers[op];
    o->op = (uint8_t)op;
    o->t1 = o1.kind; o->a1 = (uint32_t)o1.slot;
    o->t2 = o2.kind; o->a2 = (uint32_t)o2.slot;
    o->tr = ar.kind; o->ar = (uint32_t)ar.slot;
    o->line = (uint16_t)line;
    return fn->n_ops++;
}

static void patch_jump(Ctx *c, int at, uint32_t target) {
    c->fn->ops[at].ar = target;
}

static int cur_pc(Ctx *c) { return c->fn->n_ops; }

/* ---- literals ---- */
static uint32_t push_lit(Ctx *c, TVal v) {
    TFunc *fn = c->fn;
    fn->lits = tm_realloc(fn->lits, sizeof(TVal) * (size_t)(fn->n_lits + 1));
    fn->lits[fn->n_lits] = v;
    return (uint32_t)fn->n_lits++;
}

static uint32_t lit_int(Ctx *c, int64_t i) { return push_lit(c, TV_INT(i)); }
static uint32_t lit_bool(Ctx *c, bool b) { return push_lit(c, b ? TV_TRUE : TV_FALSE); }
static uint32_t lit_null(Ctx *c) { return push_lit(c, TV_NULL); }
static uint32_t lit_str(Ctx *c, TStr *s) {
    if (!(s->h.flags & GC_FLAG_INTERNED))
        s = tstr_intern(tstr_val(s), tstr_len(s));
    return push_lit(c, TV_STR(s));
}

/* ---- variables ---- */
static bool name_is_global(Ctx *c, TStr *name) {
    for (int i = 0; i < c->n_global_names; i++)
        if (c->global_names[i] == name) return true;
    return false;
}

/* get or create a CV slot for a plain local (never called for globals) */
static int get_var(Ctx *c, TStr *name) {
    for (int i = 0; i < c->n_vars; i++)
        if (c->vars[i].name == name) return c->vars[i].slot;
    if (c->n_vars == c->cap_vars) {
        c->cap_vars = c->cap_vars ? c->cap_vars * 2 : 16;
        c->vars = tm_realloc(c->vars, sizeof(*c->vars) * (size_t)c->cap_vars);
    }
    TFunc *fn = c->fn;
    int slot = fn->n_locals++;
    fn->local_names = tm_realloc(fn->local_names,
                                 sizeof(TStr *) * (size_t)fn->n_locals);
    fn->local_names[slot] = name;
    fn->is_global = tm_realloc(fn->is_global, sizeof(bool) * (size_t)fn->n_locals);
    fn->is_global[slot] = false;
    c->vars[c->n_vars].name = name;
    c->vars[c->n_vars].slot = slot;
    c->vars[c->n_vars].is_global = false;
    c->n_vars++;
    return slot;
}

static int tmp_new(Ctx *c) { return c->n_tmps++; }

/* mark a variable as a `global` alias inside the current function */
static void mark_global_var(Ctx *c, TStr *name) {
    for (int i = 0; i < c->n_vars; i++) {
        if (c->vars[i].name == name) { c->vars[i].is_global = true; return; }
    }
    if (c->n_vars == c->cap_vars) {
        c->cap_vars = c->cap_vars ? c->cap_vars * 2 : 16;
        c->vars = tm_realloc(c->vars, sizeof(*c->vars) * (size_t)c->cap_vars);
    }
    c->vars[c->n_vars].name = name;
    c->vars[c->n_vars].slot = -1;
    c->vars[c->n_vars].is_global = true;
    c->n_vars++;
}

/* main-frame variables ARE globals; only function frames use aliases */
static bool var_is_global(Ctx *c, TStr *name) {
    if (c->is_main) return false;
    for (int i = 0; i < c->n_vars; i++)
        if (c->vars[i].name == name) return c->vars[i].is_global;
    return name_is_global(c, name);
}

/* relocate virtual TMP operands after n_locals is final */
static void ctx_finish(Ctx *c) {
    TFunc *fn = c->fn;
    fn->n_tmps = (uint16_t)c->n_tmps;
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *o = &fn->ops[i];
        if (o->t1 == OPD_TMP) o->a1 += fn->n_locals;
        if (o->t2 == OPD_TMP) o->a2 += fn->n_locals;
        if (o->tr == OPD_TMP) o->ar += fn->n_locals;
    }
}

static AStmtList *asl_new_c(void) {
    AStmtList *l = tm_malloc(sizeof(AStmtList));
    l->stmts = NULL; l->n = 0;
    return l;
}

static AStmt *as_new_c(int tag, int line) {
    AStmt *s = tm_malloc(sizeof(AStmt));
    memset(s, 0, sizeof *s);
    s->tag = tag;
    s->line = line;
    return s;
}

static void asl_push_c(AStmtList *l, AStmt *s) {
    l->stmts = tm_realloc(l->stmts, sizeof(AStmt *) * (size_t)(l->n + 1));
    l->stmts[l->n++] = s;
}

static AExprList *ael_new_c(void) {
    AExprList *l = tm_malloc(sizeof(AExprList));
    l->items = NULL; l->n = 0;
    return l;
}

static AExpr *ae_new_c(int tag, int line) {
    AExpr *e = tm_malloc(sizeof(AExpr));
    memset(e, 0, sizeof *e);
    e->tag = tag;
    e->line = line;
    return e;
}

static void ael_push_c(AExprList *l, AExpr *e) {
    l->items = tm_realloc(l->items, sizeof(AExpr *) * (size_t)(l->n + 1));
    l->items[l->n++] = e;
}

/* ============ constant folding for default values ============
 * eval_const2: failable fold — *ok = false when the expression references
 * an unknown (user) constant; such defaults compile as runtime thunks. */
static TVal eval_const2(Ctx *c, AExpr *e, bool *ok) {
    switch (e->tag) {
    case A_INT: return TV_INT(e->u.i);
    case A_FLOAT: return TV_FLOAT(e->u.d);
    case A_STR: return TV_STR(tstr_intern(tstr_val(e->u.name), tstr_len(e->u.name)));
    case A_TRUE: return TV_TRUE;
    case A_FALSE: return TV_FALSE;
    case A_NULL: return TV_NULL;
    case A_CONSTREF: {
        if (e->u.name == tstr_intern("PHP_INT_MAX", 11)) return TV_INT(INT64_MAX);
        if (e->u.name == tstr_intern("PHP_INT_MIN", 11)) return TV_INT(INT64_MIN);
        if (e->u.name == tstr_intern("PHP_INT_SIZE", 12)) return TV_INT(8);
        if (e->u.name == tstr_intern("PHP_FLOAT_EPSILON", 17))
            return TV_FLOAT(2.220446049250313e-16);
        if (e->u.name == tstr_intern("INF", 3)) return TV_FLOAT(INFINITY);
        if (e->u.name == tstr_intern("NAN", 3)) return TV_FLOAT(NAN);
        *ok = false;                          /* user constant: runtime */
        return TV_NULL;
    }
    case A_UNARY:
        if (e->u.un.op == P_MINUS) {
            TVal v = eval_const2(c, e->u.un.e, ok);
            if (!*ok) return TV_NULL;
            if (v.t == T_INT) return TV_INT(-v.u.i);
            if (v.t == T_FLOAT) return TV_FLOAT(-v.u.d);
        }
        if (e->u.un.op == P_PLUS) return eval_const2(c, e->u.un.e, ok);
        *ok = false;
        return TV_NULL;
    case A_BINARY: {
        int op = e->u.bin.op;
        if (op == P_DOT) {
            TVal l = eval_const2(c, e->u.bin.l, ok);
            if (!*ok) return TV_NULL;
            TVal r = eval_const2(c, e->u.bin.r, ok);
            if (!*ok) return TV_NULL;
            TStr *a = tval_to_str(l), *b = tval_to_str(r);
            TStr *s = tstr_concat(a, b);
            tval_release(&(TVal){ .u.p = a, .t = T_STR });
            tval_release(&(TVal){ .u.p = b, .t = T_STR });
            return TV_STR(s);
        }
        TVal l = eval_const2(c, e->u.bin.l, ok);
        if (!*ok) return TV_NULL;
        TVal r = eval_const2(c, e->u.bin.r, ok);
        if (!*ok) return TV_NULL;
        if (l.t == T_INT && r.t == T_INT) {
            switch (op) {
            case P_PLUS: return TV_INT(l.u.i + r.u.i);
            case P_MINUS: return TV_INT(l.u.i - r.u.i);
            case P_MUL: return TV_INT(l.u.i * r.u.i);
            case P_MOD:
                if (r.u.i == 0) { *ok = false; return TV_NULL; }
                return TV_INT(l.u.i % r.u.i);
            default: break;
            }
        }
        switch (op) {
        case P_PLUS: return TV_FLOAT(tval_to_float(l) + tval_to_float(r));
        case P_MINUS: return TV_FLOAT(tval_to_float(l) - tval_to_float(r));
        case P_MUL: return TV_FLOAT(tval_to_float(l) * tval_to_float(r));
        case P_DIV:
            if (tval_to_float(r) == 0.0) { *ok = false; return TV_NULL; }
            return TV_FLOAT(tval_to_float(l) / tval_to_float(r));
        default:
            *ok = false;
            return TV_NULL;
        }
    }
    case A_ARRAY: {
        /* array literals are valid constant expressions (class consts,
         * static-prop defaults): fold them eagerly, no spread */
        AArrayList *al = e->u.alist;
        TArr *a = tarr_new();
        int64_t next = 0;
        for (int i = 0; i < al->n; i++) {
            AArrayItem *it = &al->items[i];
            if (it->is_spread) { *ok = false; tarr_free(a); return TV_NULL; }
            TVal v = eval_const2(c, it->val, ok);
            if (!*ok) { tarr_free(a); return TV_NULL; }
            if (it->key) {
                TVal k = eval_const2(c, it->key, ok);
                if (!*ok) { tval_release(&v); tarr_free(a); return TV_NULL; }
                if (!tval_norm_key(&k)) {
                    *ok = false;
                    tval_release(&v); tarr_free(a);
                    return TV_NULL;
                }
                if (k.t == T_INT) next = k.u.i + 1;
                tarr_set(a, k, v);          /* value ownership moves in */
            } else {
                tarr_set(a, TV_INT(next), v);
                next++;
            }
        }
        return TV_ARR(a);
    }
    default:
        *ok = false;
        return TV_NULL;
    }
}

static TVal eval_const(Ctx *c, AExpr *e) {
    bool ok = true;
    TVal v = eval_const2(c, e, &ok);
    if (!ok)
        tphp_fatal_src(c->file, e->line,
                       "Constant expression contains unknown constants in M1");
    return v;
}

/* ============ expressions ============ */
static Loc compile_expr(Ctx *c, AExpr *e, Loc want);
static Loc compile_closure_expr(Ctx *c, AExpr *e, Loc want);
static void resolve_gotos(Ctx *c, const char *file);

static Loc tmp_dst(Ctx *c, Loc want) {
    return want.kind != OPD_UNUSED ? want : LOC_TMP(tmp_new(c));
}

static Loc compile_var_read(Ctx *c, TStr *name, int line, Loc want) {
    if (!c->is_main && var_is_global(c, name)) {
        Loc dst = tmp_dst(c, want);
        emit(c, OP_FETCH_GLOBAL, LOC_NONE, LOC_CONST(lit_str(c, name)), dst, line);
        return dst;
    }
    int slot = get_var(c, name);
    if (want.kind != OPD_UNUSED && !(want.kind == OPD_CV && want.slot == slot)) {
        emit(c, OP_COPY, LOC_CV(slot), LOC_NONE, want, line);
        return want;
    }
    return LOC_CV(slot);
}

/* silent read for ?? / empty / isset: undefined globals read as null,
 * undefined locals leave their T_UNDEF slot for JMP_NULL / BOOL_VAL */
static Loc compile_var_read_silent(Ctx *c, TStr *name, int line) {
    if (!c->is_main && var_is_global(c, name)) {
        Loc t = LOC_TMP(tmp_new(c));
        emit(c, OP_ISSET_GLOBAL, LOC_CONST(lit_str(c, name)), LOC_NONE, t, line);
        int jz = emit(c, OP_JMPZ, t, LOC_NONE, LOC_NONE, line);
        Loc v = LOC_TMP(tmp_new(c));
        emit(c, OP_FETCH_GLOBAL, LOC_NONE, LOC_CONST(lit_str(c, name)), v, line);
        int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, line);
        patch_jump(c, jz, (uint32_t)cur_pc(c));
        emit(c, OP_CONST, LOC_CONST(lit_null(c)), LOC_NONE, v, line);
        patch_jump(c, jend, (uint32_t)cur_pc(c));
        return v;
    }
    return LOC_CV(get_var(c, name));
}

static Loc compile_andor(Ctx *c, AExpr *e, Loc want, bool is_and) {
    Loc dst = tmp_dst(c, want);
    Loc l = compile_expr(c, e->u.bin.l, LOC_NONE);
    int t1 = tmp_new(c);
    emit(c, OP_BOOL_VAL, l, LOC_NONE, LOC_TMP(t1), e->line);
    int jshort = emit(c, is_and ? OP_JMPZ : OP_JMPNZ, LOC_TMP(t1), LOC_NONE, LOC_NONE, e->line);
    Loc r = compile_expr(c, e->u.bin.r, LOC_NONE);
    int t2 = tmp_new(c);
    emit(c, OP_BOOL_VAL, r, LOC_NONE, LOC_TMP(t2), e->line);
    emit(c, OP_COPY, LOC_TMP(t2), LOC_NONE, dst, e->line);
    int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
    patch_jump(c, jshort, (uint32_t)cur_pc(c));
    uint32_t lv = lit_bool(c, !is_and); /* and-shortcircuit = false; or = true */
    emit(c, OP_CONST, LOC_CONST(lv), LOC_NONE, dst, e->line);
    patch_jump(c, jend, (uint32_t)cur_pc(c));
    return dst;
}

/* DIM read; silent=true yields null for missing keys / undefined bases */
static Loc compile_dim_read(Ctx *c, AExpr *e, bool silent, Loc want) {
    Loc base = compile_expr(c, e->u.dim.base, LOC_NONE);
    Loc idx = LOC_NONE;
    if (e->u.dim.idx)
        idx = compile_expr(c, e->u.dim.idx, LOC_NONE);
    Loc dst = tmp_dst(c, want);
    emit(c, silent ? OP_FETCH_DIM_IS : OP_FETCH_DIM_R, base, idx, dst, e->line);
    return dst;
}

/* isset($a[$i][...]): silent reads along the chain, bool verdict */
static Loc compile_dim_isset(Ctx *c, AExpr *a) {
    Loc base;
    if (a->u.dim.base->tag == A_DIM)
        base = compile_dim_read(c, a->u.dim.base, true, LOC_NONE);
    else if (a->u.dim.base->tag == A_VAR)
        base = compile_var_read_silent(c, a->u.dim.base->u.name, a->u.dim.base->line);
    else
        base = compile_expr(c, a->u.dim.base, LOC_NONE);
    Loc idx = LOC_NONE;
    if (a->u.dim.idx)
        idx = compile_expr(c, a->u.dim.idx, LOC_NONE);
    Loc r = LOC_TMP(tmp_new(c));
    emit(c, OP_ISSET_DIM, base, idx, r, a->line);
    return r;
}

/* ---- writable dim chains ---- */
typedef struct WBList {
    Loc base, idx, val;      /* store val into base[idx] after the mutation */
    bool is_global;
    bool is_prop;            /* base is $obj->prop (ASSIGN_PROP writeback) */
    bool is_sprop;           /* base is C::$prop (ASSIGN_SPROP writeback) */
    int skind;               /* static-prop class kind (0/1/2/3) */
    TStr *gname;
    TStr *pname;
    TStr *skey;              /* static-prop lit key ("Cls::$prop"/"$prop") */
    struct WBList *next;
} WBList;

/* compile the base of a writable dim chain; returns the slot the mutating
 * handler may rewrite (COW separation / auto-vivification). Pushes one
 * write-back per level (outermost first; the natural list order is
 * innermost-first which is the required execution order). */
/* P2-A nullsafe: does this expr's property/method access chain (obj side)
 * contain a ?-> hop? Once a chain enters nullsafe mode, every later hop is
 * implicitly null-guarded (PHP: the whole chain short-circuits to null) */
static bool chain_has_nullsafe(AExpr *e) {
    for (;;) {
        if (!e) return false;
        if (e->tag == A_PROP) {
            if (e->nullsafe) return true;
            e = e->u.prop.obj;
            continue;
        }
        if (e->tag == A_METHODCALL) {
            if (e->nullsafe) return true;
            e = e->u.mcall.obj;
            continue;
        }
        return false;
    }
}

static Loc compile_lvalue_base(Ctx *c, AExpr *e, WBList **wb, int line) {
    if (e->tag == A_VAR) {
        if (!c->is_main && var_is_global(c, e->u.name)) {
            Loc gv = LOC_TMP(tmp_new(c));
            emit(c, OP_FETCH_GLOBAL, LOC_NONE, LOC_CONST(lit_str(c, e->u.name)), gv, e->line);
            WBList *w = tm_malloc(sizeof *w);
            memset(w, 0, sizeof *w);
            w->is_global = true;
            w->gname = e->u.name;
            w->val = gv;
            w->next = *wb;
            *wb = w;
            return gv;
        }
        return LOC_CV(get_var(c, e->u.name));
    }
    if (e->tag == A_PROP) {
        /* $obj->prop[...] = v : fetch the prop into a tmp, mutate the dim,
         * write the tmp back through ASSIGN_PROP (__set hooks apply) */
        Loc b = compile_expr(c, e->u.prop.obj, LOC_NONE);
        Loc mid = LOC_TMP(tmp_new(c));
        emit(c, OP_FETCH_PROP, b, LOC_CONST(lit_str(c, e->u.prop.name)),
             mid, e->line);
        WBList *w = tm_malloc(sizeof *w);
        memset(w, 0, sizeof *w);
        w->base = b; w->val = mid;
        w->is_prop = true;
        w->pname = e->u.prop.name;
        w->next = *wb;
        *wb = w;
        return mid;
    }
    if (e->tag == A_STATICGET && tstr_val(e->u.scall.member)[0] == '$') {
        /* C::$prop[...] = v : fetch the static prop into a tmp, mutate the
         * dim, write back through ASSIGN_SPROP */
        TStr *ccls = e->u.scall.cls;
        const char *raw = tstr_val(ccls);
        bool is_sp = tstr_case_eq(raw, "self") || tstr_case_eq(raw, "parent") ||
                     tstr_case_eq(raw, "static");
        if (!is_sp) ccls = resolve_class_name(ccls);
        const char *cn = tstr_val(ccls);
        int kind = tstr_case_eq(cn, "self") ? 0 :
                   tstr_case_eq(cn, "parent") ? 1 :
                   tstr_case_eq(cn, "static") ? 2 : 3;
        char key[256];
        if (kind == 3)
            snprintf(key, sizeof key, "%s::%s", cn, tstr_val(e->u.scall.member));
        else
            snprintf(key, sizeof key, "%s", tstr_val(e->u.scall.member));
        TStr *keystr = tstr_intern(key, strlen(key));
        Loc mid = LOC_TMP(tmp_new(c));
        emit(c, OP_FETCH_CCONST, (Loc){ .kind = OPD_UNUSED, .slot = kind },
             LOC_CONST(lit_str(c, keystr)), mid, e->line);
        WBList *w = tm_malloc(sizeof *w);
        memset(w, 0, sizeof *w);
        w->val = mid;
        w->is_sprop = true;
        w->skind = kind;
        w->skey = keystr;
        w->next = *wb;
        *wb = w;
        return mid;
    }
    if (e->tag == A_DIM) {
        Loc b = compile_lvalue_base(c, e->u.dim.base, wb, e->line);
        Loc i = LOC_NONE;
        if (e->u.dim.idx)
            i = compile_expr(c, e->u.dim.idx, LOC_NONE);
        Loc mid = LOC_TMP(tmp_new(c));
        emit(c, OP_FETCH_DIM_R, b, i, mid, e->line);
        WBList *w = tm_malloc(sizeof *w);
        memset(w, 0, sizeof *w);   /* MUST zero: is_prop/is_sprop garbage
                                    * made emit_writebacks deref wild
                                    * pointers (Linux-only SIGSEGV) */
        w->base = b; w->idx = i; w->val = mid;
        w->next = *wb;
        *wb = w;
        return mid;
    }
    tphp_fatal_src(c->file, e->line, "Cannot use this expression as a writable target");
}

static void emit_writebacks(Ctx *c, WBList *wb, int line) {
    while (wb) {
        WBList *next = wb->next;
        if (wb->is_global)
            emit(c, OP_ASSIGN_GLOBAL, wb->val, LOC_CONST(lit_str(c, wb->gname)), LOC_NONE, line);
        else if (wb->is_sprop)
            emit(c, OP_ASSIGN_SPROP, (Loc){ .kind = OPD_UNUSED, .slot = wb->skind },
                 LOC_CONST(lit_str(c, wb->skey)), wb->val, line);
        else if (wb->is_prop)
            emit(c, OP_ASSIGN_PROP, wb->base, LOC_CONST(lit_str(c, wb->pname)),
                 wb->val, line);
        else
            emit(c, OP_ASSIGN_DIM, wb->base, wb->idx, wb->val, line);
        tm_free(wb);
        wb = next;
    }
}

static FnSig *find_sig(Ctx *c, TStr *name) {
    for (int i = 0; i < c->n_sigs; i++)
        if (c->sigs[i].name == name) return &c->sigs[i];
    return NULL;
}

static uint32_t lit_undef(Ctx *c) {
    return push_lit(c, (TVal){ .u.i = 0, .t = T_UNDEF });
}

/* P2-A named args: shared reordering for static/self/parent calls and new
 * (compile-time signature known). Every declared parameter slot receives a
 * value or a T_UNDEF sentinel (RECV falls back to the default); extras go
 * through for variadic use. param_names come from TMethod (AST interned,
 * pointer-comparable with AArg.name). Returns the arg count sent. */
static int compile_named_sends(Ctx *c, int line, AArgList *args,
                               TStr **param_names, uint16_t n_params) {
    int next_pos = 0;
    bool *consumed = NULL;
    if (args->n) {
        consumed = tm_malloc(sizeof(bool) * (size_t)args->n);
        memset(consumed, 0, sizeof(bool) * (size_t)args->n);
    }
    for (uint16_t pi = 0; pi < n_params; pi++) {
        AArg *named = NULL;
        for (int i = 0; i < args->n; i++)
            if (!consumed[i] && args->items[i].name &&
                args->items[i].name == param_names[pi]) {
                named = &args->items[i];
                consumed[i] = true;
                break;
            }
        if (named) {
            Loc v = compile_expr(c, named->e, LOC_NONE);
            emit(c, OP_SEND_VAL, v, LOC_NONE, LOC_NONE, line);
        } else {
            while (next_pos < args->n &&
                   (consumed[next_pos] || args->items[next_pos].name))
                next_pos++;
            if (next_pos < args->n && !args->items[next_pos].is_spread) {
                Loc v = compile_expr(c, args->items[next_pos].e, LOC_NONE);
                emit(c, OP_SEND_VAL, v, LOC_NONE, LOC_NONE, line);
                consumed[next_pos] = true;
                next_pos++;
            } else {
                emit(c, OP_SEND_VAL, LOC_CONST(lit_undef(c)), LOC_NONE,
                     LOC_NONE, line);
            }
        }
    }
    /* any unconsumed named arg matched no parameter -> unknown (PHP: Error);
     * must be checked before the extras loop skips it silently */
    for (int i = 0; i < args->n; i++)
        if (!consumed[i] && args->items[i].name)
            tphp_fatal_src(c->file, line, "Unknown named parameter %s",
                           tstr_val(args->items[i].name));
    while (next_pos < args->n) {           /* extra args (variadic use) */
        if (consumed[next_pos]) { next_pos++; continue; }
        Loc v = compile_expr(c, args->items[next_pos].e, LOC_NONE);
        emit(c, OP_SEND_VAL, v, LOC_NONE, LOC_NONE, line);
        consumed[next_pos] = true;
        next_pos++;
    }
    if (consumed) tm_free(consumed);
    return (int)n_params;
}

static Loc compile_call(Ctx *c, AExpr *e, Loc want) {
    Loc dst = tmp_dst(c, want);
    AArgList *args = e->u.call.args;
    bool has_named = false, has_spread = false;
    for (int i = 0; i < args->n; i++) {
        if (args->items[i].name) has_named = true;
        if (args->items[i].is_spread) has_spread = true;
    }
    int init_at;
    TStr *fname = NULL;
    if (e->u.call.dyn) {
        if (has_named)
            tphp_fatal_src(c->file, e->line,
                           "Named arguments are not supported for variable calls in M2");
        Loc fv = compile_expr(c, e->u.call.dyn, LOC_NONE);
        init_at = emit(c, OP_INIT_DYN, fv, LOC_NONE, LOC_NONE, e->line);
    } else {
        bool fb;
        fname = resolve_func_name(e->u.call.name, &fb);
        init_at = emit(c, OP_INIT_FCALL, LOC_CONST(lit_str(c, fname)),
                       LOC_NONE, LOC_NONE, e->line);
        if (fb) c->fn->ops[init_at].ext = 1;   /* runtime bare-name fallback */
    }
    int n_sent = 0;
    if (has_named) {
        /* named-argument reordering: every parameter slot receives a value
         * or a T_UNDEF sentinel (RECV falls back to the default) */
        if (has_spread)
            tphp_fatal_src(c->file, e->line,
                           "Cannot combine named arguments and spread in M2");
        FnSig *sig = e->u.call.dyn ? NULL : find_sig(c, fname);
        if (!sig)
            tphp_fatal_src(c->file, e->line,
                           "Named arguments are only supported for user functions in M2");
        int next_pos = 0;
        bool *consumed = NULL;
        if (args->n) {
            consumed = tm_malloc(sizeof(bool) * (size_t)args->n);
            memset(consumed, 0, sizeof(bool) * (size_t)args->n);
        }
        for (int pi = 0; pi < sig->n_params; pi++) {
            AArg *named = NULL;
            for (int i = 0; i < args->n; i++)
                if (!consumed[i] && args->items[i].name && args->items[i].name == sig->params[pi]) {
                    named = &args->items[i];
                    consumed[i] = true;
                    break;
                }
            if (named) {
                Loc v = compile_expr(c, named->e, LOC_NONE);
                emit(c, OP_SEND_VAL, v, LOC_NONE, LOC_NONE, e->line);
            } else {
                while (next_pos < args->n && (consumed[next_pos] || args->items[next_pos].name))
                    next_pos++;
                if (next_pos < args->n && !args->items[next_pos].is_spread) {
                    Loc v = compile_expr(c, args->items[next_pos].e, LOC_NONE);
                    emit(c, OP_SEND_VAL, v, LOC_NONE, LOC_NONE, e->line);
                    consumed[next_pos] = true;
                    next_pos++;
                } else {
                    emit(c, OP_SEND_VAL, LOC_CONST(lit_undef(c)), LOC_NONE, LOC_NONE, e->line);
                }
            }
            n_sent++;
        }
        /* unconsumed named args matched no parameter -> unknown (PHP: Error);
         * checked before the extras loop, which would skip them silently */
        for (int i = 0; i < args->n; i++)
            if (!consumed[i] && args->items[i].name)
                tphp_fatal_src(c->file, e->line, "Unknown named parameter %s",
                               tstr_val(args->items[i].name));
        while (next_pos < args->n) {           /* extra args (variadic use) */
            if (consumed[next_pos]) { next_pos++; continue; }
            Loc v = compile_expr(c, args->items[next_pos].e, LOC_NONE);
            emit(c, OP_SEND_VAL, v, LOC_NONE, LOC_NONE, e->line);
            consumed[next_pos] = true;
            next_pos++;
            n_sent++;
        }
        if (consumed) tm_free(consumed);
    } else {
        for (int i = 0; i < args->n; i++) {
            AArg *arg = &args->items[i];
            if (arg->is_spread) {
                Loc sv = compile_expr(c, arg->e, LOC_NONE);
                emit(c, OP_SEND_SPREAD, sv, LOC_NONE, LOC_NONE, e->line);
            } else {
                Loc av = compile_expr(c, arg->e, LOC_NONE);
                emit(c, OP_SEND_VAL, av, LOC_NONE, LOC_NONE, e->line);
                n_sent++;
            }
        }
    }
    c->fn->ops[init_at].a2 = (uint32_t)n_sent;
    emit(c, OP_DO_FCALL, LOC_NONE, LOC_NONE, dst, e->line);
    return dst;
}

static Loc compile_assign(Ctx *c, AExpr *e, Loc want) {
    AExpr *t = e->u.bin.l, *v = e->u.bin.r;
    if (t->tag == A_VAR) {
        if (tstr_case_eq(tstr_val(t->u.name), "this"))
            tphp_fatal_src(c->file, t->line, "Cannot re-assign $this");
        if (!c->is_main && var_is_global(c, t->u.name)) {
            Loc rv = compile_expr(c, v, LOC_NONE);
            emit(c, OP_ASSIGN_GLOBAL, rv, LOC_CONST(lit_str(c, t->u.name)), LOC_NONE, e->line);
            if (want.kind != OPD_UNUSED) {
                emit(c, OP_COPY, rv, LOC_NONE, want, e->line);
                return want;
            }
            return rv;
        }
        int slot = get_var(c, t->u.name);
        compile_expr(c, v, LOC_CV(slot));   /* compile straight into the CV */
        if (want.kind != OPD_UNUSED && !(want.kind == OPD_CV && want.slot == slot)) {
            emit(c, OP_COPY, LOC_CV(slot), LOC_NONE, want, e->line);
            return want;
        }
        return LOC_CV(slot);
    }
    if (t->tag == A_DIM) {
        WBList *wb = NULL;
        Loc base = compile_lvalue_base(c, t->u.dim.base, &wb, t->line);
        Loc idx = LOC_NONE;
        if (t->u.dim.idx)
            idx = compile_expr(c, t->u.dim.idx, LOC_NONE);
        Loc rv = compile_expr(c, v, LOC_NONE);
        emit(c, OP_ASSIGN_DIM, base, idx, rv, e->line);
        emit_writebacks(c, wb, e->line);
        if (want.kind != OPD_UNUSED) {
            emit(c, OP_COPY, rv, LOC_NONE, want, e->line);
            return want;
        }
        return rv;
    }
    if (t->tag == A_PROP) {
        Loc obj = compile_expr(c, t->u.prop.obj, LOC_NONE);
        Loc rv = compile_expr(c, v, LOC_NONE);
        emit(c, OP_ASSIGN_PROP, obj, LOC_CONST(lit_str(c, t->u.prop.name)), rv, e->line);
        if (want.kind != OPD_UNUSED) {
            emit(c, OP_COPY, rv, LOC_NONE, want, e->line);
            return want;
        }
        return rv;
    }
    if (t->tag == A_STATICGET && tstr_val(t->u.scall.member)[0] == '$') {
        /* C::$prop = v / self::$prop = v (member keeps the leading '$') */
        TStr *ccls = t->u.scall.cls;
        const char *raw = tstr_val(ccls);
        bool is_sp = tstr_case_eq(raw, "self") || tstr_case_eq(raw, "parent") ||
                     tstr_case_eq(raw, "static");
        if (!is_sp) ccls = resolve_class_name(ccls);
        const char *cn = tstr_val(ccls);
        int kind = tstr_case_eq(cn, "self") ? 0 :
                   tstr_case_eq(cn, "parent") ? 1 :
                   tstr_case_eq(cn, "static") ? 2 : 3;
        char key[256];
        if (kind == 3)
            snprintf(key, sizeof key, "%s::%s", cn, tstr_val(t->u.scall.member));
        else
            snprintf(key, sizeof key, "%s", tstr_val(t->u.scall.member));
        Loc rv = compile_expr(c, v, LOC_NONE);
        emit(c, OP_ASSIGN_SPROP, (Loc){ .kind = OPD_UNUSED, .slot = kind },
             LOC_CONST(lit_str(c, tstr_intern(key, strlen(key)))), rv, e->line);
        if (want.kind != OPD_UNUSED) {
            emit(c, OP_COPY, rv, LOC_NONE, want, e->line);
            return want;
        }
        return rv;
    }
    tphp_fatal_src(c->file, t->line, "Cannot assign to this expression");
}

static int assign_op_base(int op) {
    switch (op) {
    case P_PLUS_EQ: return P_PLUS;
    case P_MINUS_EQ: return P_MINUS;
    case P_MUL_EQ: return P_MUL;
    case P_DIV_EQ: return P_DIV;
    case P_MOD_EQ: return P_MOD;
    case P_POW_EQ: return P_POW;
    case P_DOT_EQ: return P_DOT;
    case P_AND_EQ: return P_AMP;
    case P_OR_EQ: return P_PIPE;
    case P_XOR_EQ: return P_CARET;
    case P_SHL_EQ: return P_SHL;
    case P_SHR_EQ: return P_SHR;
    }
    return -1;
}

static Opcode bin_opcode(int op) {
    switch (op) {
    case P_PLUS: return OP_ADD;
    case P_MINUS: return OP_SUB;
    case P_MUL: return OP_MUL;
    case P_DIV: return OP_DIV;
    case P_MOD: return OP_MOD;
    case P_POW: return OP_POW;
    case P_DOT: return OP_CONCAT;
    case P_AMP: return OP_BW_AND;
    case P_PIPE: return OP_BW_OR;
    case P_CARET: return OP_BW_XOR;
    case P_SHL: return OP_SHL;
    case P_SHR: return OP_SHR;
    case P_EQ: return OP_EQ;
    case P_NE: return OP_NE;
    case P_SEQ: return OP_SEQ;
    case P_SNE: return OP_SNE;
    case P_SPACESHIP: return OP_CMP;
    case P_LT: return OP_LT;
    case P_LE: return OP_LE;
    case P_GT: return OP_LT;   /* operands swapped by the caller */
    case P_GE: return OP_LE;
    }
    tphp_fatal_src(NULL, 0, "Internal: unknown binary operator");
}

/* $x op= v compiles to: <op> $x, v -> $x (one instruction, slot RMW);
 * $a[i] op= v: FETCH_DIM_R, <op> t, v -> t, ASSIGN_DIM back. */
static Loc compile_assign_op(Ctx *c, AExpr *e, Loc want) {
    int base = assign_op_base(e->u.bin.op);
    if (base < 0)
        tphp_fatal_src(c->file, e->line, "Internal: unknown compound assignment");
    Opcode opc = bin_opcode(base);
    AExpr *t = e->u.bin.l, *v = e->u.bin.r;
    if (t->tag == A_VAR) {
        if (tstr_case_eq(tstr_val(t->u.name), "this"))
            tphp_fatal_src(c->file, t->line, "Cannot re-assign $this");
        if (!c->is_main && var_is_global(c, t->u.name)) {
            Loc gv = LOC_TMP(tmp_new(c));
            emit(c, OP_FETCH_GLOBAL, LOC_NONE, LOC_CONST(lit_str(c, t->u.name)), gv, e->line);
            Loc rv = compile_expr(c, v, LOC_NONE);
            emit(c, opc, gv, rv, gv, e->line);
            emit(c, OP_ASSIGN_GLOBAL, gv, LOC_CONST(lit_str(c, t->u.name)), LOC_NONE, e->line);
            if (want.kind != OPD_UNUSED) {
                emit(c, OP_COPY, gv, LOC_NONE, want, e->line);
                return want;
            }
            return gv;
        }
        int slot = get_var(c, t->u.name);
        Loc rv = compile_expr(c, v, LOC_NONE);
        emit(c, opc, LOC_CV(slot), rv, LOC_CV(slot), e->line);
        if (want.kind != OPD_UNUSED && !(want.kind == OPD_CV && want.slot == slot)) {
            emit(c, OP_COPY, LOC_CV(slot), LOC_NONE, want, e->line);
            return want;
        }
        return LOC_CV(slot);
    }
    if (t->tag == A_DIM) {
        WBList *wb = NULL;
        Loc base_l = compile_lvalue_base(c, t->u.dim.base, &wb, t->line);
        Loc idx = LOC_NONE;
        if (t->u.dim.idx)
            idx = compile_expr(c, t->u.dim.idx, LOC_NONE);
        Loc mid = LOC_TMP(tmp_new(c));
        emit(c, OP_FETCH_DIM_R, base_l, idx, mid, e->line);
        Loc rv = compile_expr(c, v, LOC_NONE);
        emit(c, opc, mid, rv, mid, e->line);
        emit(c, OP_ASSIGN_DIM, base_l, idx, mid, e->line);
        emit_writebacks(c, wb, e->line);
        if (want.kind != OPD_UNUSED) {
            emit(c, OP_COPY, mid, LOC_NONE, want, e->line);
            return want;
        }
        return mid;
    }
    if (t->tag == A_PROP) {
        /* $obj->p op= v : read-modify-write through a tmp */
        Loc obj = compile_expr(c, t->u.prop.obj, LOC_NONE);
        Loc mid = LOC_TMP(tmp_new(c));
        emit(c, OP_FETCH_PROP, obj, LOC_CONST(lit_str(c, t->u.prop.name)), mid, e->line);
        Loc rv = compile_expr(c, v, LOC_NONE);
        emit(c, opc, mid, rv, mid, e->line);
        emit(c, OP_ASSIGN_PROP, obj, LOC_CONST(lit_str(c, t->u.prop.name)), mid, e->line);
        if (want.kind != OPD_UNUSED) {
            emit(c, OP_COPY, mid, LOC_NONE, want, e->line);
            return want;
        }
        return mid;
    }
    tphp_fatal_src(c->file, t->line, "Cannot assign to this expression");
}

static Loc compile_expr(Ctx *c, AExpr *e, Loc want) {
    switch (e->tag) {
    case A_INT: {
        Loc dst = tmp_dst(c, want);
        emit(c, OP_CONST, LOC_CONST(lit_int(c, e->u.i)), LOC_NONE, dst, e->line);
        return dst;
    }
    case A_FLOAT: {
        Loc dst = tmp_dst(c, want);
        emit(c, OP_CONST, LOC_CONST(push_lit(c, TV_FLOAT(e->u.d))), LOC_NONE, dst, e->line);
        return dst;
    }
    case A_STR: {
        Loc dst = tmp_dst(c, want);
        emit(c, OP_CONST, LOC_CONST(lit_str(c, e->u.name)), LOC_NONE, dst, e->line);
        return dst;
    }
    case A_TRUE: case A_FALSE: case A_NULL: {
        Loc dst = tmp_dst(c, want);
        uint32_t li = e->tag == A_TRUE ? lit_bool(c, true)
                    : e->tag == A_FALSE ? lit_bool(c, false)
                    : lit_null(c);
        emit(c, OP_CONST, LOC_CONST(li), LOC_NONE, dst, e->line);
        return dst;
    }
    case A_VAR:
        return compile_var_read(c, e->u.name, e->line, want);
    case A_CONSTREF: {
        Loc dst = tmp_dst(c, want);
        /* built-in constants resolve at load time */
        static TStr *s_pim, *s_pimin, *s_pisz, *s_eps, *s_inf, *s_nan;
        if (!s_pim) {
            s_pim = tstr_intern("PHP_INT_MAX", 11);
            s_pimin = tstr_intern("PHP_INT_MIN", 11);
            s_pisz = tstr_intern("PHP_INT_SIZE", 12);
            s_eps = tstr_intern("PHP_FLOAT_EPSILON", 17);
            s_inf = tstr_intern("INF", 3);
            s_nan = tstr_intern("NAN", 3);
        }
        uint32_t li;
        if (e->u.name == s_pim) li = lit_int(c, INT64_MAX);
        else if (e->u.name == s_pimin) li = lit_int(c, INT64_MIN);
        else if (e->u.name == s_pisz) li = lit_int(c, 8);
        else if (e->u.name == s_eps) li = push_lit(c, TV_FLOAT(2.220446049250313e-16));
        else if (e->u.name == s_inf) li = push_lit(c, TV_FLOAT(INFINITY));
        else if (e->u.name == s_nan) li = push_lit(c, TV_FLOAT(NAN));
        else {
            bool fb;
            TStr *cname = resolve_const_name(e->u.name, &fb);
            int at = emit(c, OP_FETCH_CONST, LOC_CONST(lit_str(c, cname)),
                          LOC_NONE, dst, e->line);
            if (fb) c->fn->ops[at].ext = 1;    /* runtime bare-name fallback */
            return dst;
        }
        emit(c, OP_CONST, LOC_CONST(li), LOC_NONE, dst, e->line);
        return dst;
    }
    case A_ARRAY: {
        Loc dst = tmp_dst(c, want);
        emit(c, OP_ARRAY_INIT, LOC_NONE, LOC_NONE, dst, e->line);
        AArrayList *al = e->u.alist;
        for (int i = 0; i < al->n; i++) {
            AArrayItem *it = &al->items[i];
            if (it->is_spread) {
                Loc sv = compile_expr(c, it->val, LOC_NONE);
                emit(c, OP_ARRAY_SPREAD, dst, sv, LOC_NONE, it->val->line);
            } else {
                Loc v = compile_expr(c, it->val, LOC_NONE);
                Loc k = LOC_NONE;
                if (it->key)
                    k = compile_expr(c, it->key, LOC_NONE);
                emit(c, OP_ARRAY_APPEND, dst, k, v, it->val->line);
            }
        }
        return dst;
    }
    case A_BINARY: {
        int op = e->u.bin.op;
        if (op == P_ANDAND) return compile_andor(c, e, want, true);
        if (op == P_OROR) return compile_andor(c, e, want, false);
        Loc l = compile_expr(c, e->u.bin.l, LOC_NONE);
        Loc r = compile_expr(c, e->u.bin.r, LOC_NONE);
        if (op == P_GT || op == P_GE) { Loc t = l; l = r; r = t; }
        Loc dst = tmp_dst(c, want);
        emit(c, bin_opcode(op), l, r, dst, e->line);
        return dst;
    }
    case A_UNARY: {
        Loc v = compile_expr(c, e->u.un.e, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        Opcode opc;
        switch (e->u.un.op) {
        case P_MINUS: opc = OP_NEG; break;
        case P_PLUS: opc = OP_POS; break;
        case P_TILDE: opc = OP_BIT_NOT; break;
        case P_NOT: opc = OP_BOOL_NOT; break;
        default:
            tphp_fatal_src(c->file, e->line, "Internal: unknown unary operator");
        }
        emit(c, opc, v, LOC_NONE, dst, e->line);
        return dst;
    }
    case A_CAST: {
        Loc v = compile_expr(c, e->u.cast.e, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        emit(c, OP_CAST, v, LOC_NONE, dst, e->line);
        /* cast target rides in a2 as a CONST-pool int */
        c->fn->ops[c->fn->n_ops - 1].t2 = OPD_CONST;
        c->fn->ops[c->fn->n_ops - 1].a2 = lit_int(c, e->u.cast.ctype);
        return dst;
    }
    case A_TERNARY: {
        Loc dst = tmp_dst(c, want);
        Loc cond = compile_expr(c, e->u.tern.c, LOC_NONE);
        if (!e->u.tern.m) {
            /* short ternary: cond ?: else — cond value reused when truthy */
            int jz = emit(c, OP_JMPZ, cond, LOC_NONE, LOC_NONE, e->line);
            emit(c, OP_COPY, cond, LOC_NONE, dst, e->line);
            int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
            patch_jump(c, jz, (uint32_t)cur_pc(c));
            compile_expr(c, e->u.tern.e, dst);
            patch_jump(c, jend, (uint32_t)cur_pc(c));
            return dst;
        }
        int jz = emit(c, OP_JMPZ, cond, LOC_NONE, LOC_NONE, e->line);
        compile_expr(c, e->u.tern.m, dst);
        int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
        patch_jump(c, jz, (uint32_t)cur_pc(c));
        compile_expr(c, e->u.tern.e, dst);
        patch_jump(c, jend, (uint32_t)cur_pc(c));
        return dst;
    }
    case A_COALESCE: {
        Loc dst = tmp_dst(c, want);
        AExpr *l = e->u.tern.c;
        Loc lv;
        if (l->tag == A_VAR) {
            lv = compile_var_read_silent(c, l->u.name, l->line);
        } else if (l->tag == A_DIM) {
            lv = compile_dim_read(c, l, true, LOC_NONE);
        } else {
            lv = compile_expr(c, l, LOC_NONE);
        }
        int jn = emit(c, OP_JMP_NULL, lv, LOC_NONE, LOC_NONE, e->line);
        emit(c, OP_COPY, lv, LOC_NONE, dst, e->line);
        int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
        patch_jump(c, jn, (uint32_t)cur_pc(c));
        compile_expr(c, e->u.tern.e, dst);
        patch_jump(c, jend, (uint32_t)cur_pc(c));
        return dst;
    }
    case A_DIM:
        return compile_dim_read(c, e, false, want);
    case A_INCDEC: {
        AExpr *t = e->u.un.e;
        bool prefix = e->u.un.prefix;
        bool is_inc = e->u.un.op == P_INC;
        if (t->tag == A_VAR) {
            Loc dst = tmp_dst(c, want);
            if (!c->is_main && var_is_global(c, t->u.name)) {
                Loc gv = LOC_TMP(tmp_new(c));
                emit(c, OP_FETCH_GLOBAL, LOC_NONE, LOC_CONST(lit_str(c, t->u.name)), gv, t->line);
                emit(c, is_inc ? OP_INC : OP_DEC, gv, LOC_NONE, dst, t->line);
                c->fn->ops[c->fn->n_ops - 1].ext = prefix ? 1 : 0;
                emit(c, OP_ASSIGN_GLOBAL, gv, LOC_CONST(lit_str(c, t->u.name)), LOC_NONE, t->line);
            } else {
                int slot = get_var(c, t->u.name);
                emit(c, is_inc ? OP_INC : OP_DEC, LOC_CV(slot), LOC_NONE, dst, t->line);
                c->fn->ops[c->fn->n_ops - 1].ext = prefix ? 1 : 0;
            }
            return dst;
        }
        if (t->tag == A_DIM) {
            WBList *wb = NULL;
            Loc base = compile_lvalue_base(c, t->u.dim.base, &wb, t->line);
            Loc idx = LOC_NONE;
            if (t->u.dim.idx)
                idx = compile_expr(c, t->u.dim.idx, LOC_NONE);
            Loc dst = tmp_dst(c, want);
            emit(c, is_inc ? OP_INC : OP_DEC, base, idx, dst, t->line);
            c->fn->ops[c->fn->n_ops - 1].ext = prefix ? 1 : 0;
            emit_writebacks(c, wb, t->line);
            return dst;
        }
        if (t->tag == A_PROP) {
            /* $obj->prop++ : fetch, RMW in place, write back through
             * ASSIGN_PROP (__set hooks / readonly checks apply) */
            Loc obj = compile_expr(c, t->u.prop.obj, LOC_NONE);
            Loc mid = LOC_TMP(tmp_new(c));
            emit(c, OP_FETCH_PROP, obj, LOC_CONST(lit_str(c, t->u.prop.name)),
                 mid, t->line);
            Loc dst = tmp_dst(c, want);
            emit(c, is_inc ? OP_INC : OP_DEC, mid, LOC_NONE, dst, t->line);
            c->fn->ops[c->fn->n_ops - 1].ext = prefix ? 1 : 0;
            emit(c, OP_ASSIGN_PROP, obj, LOC_CONST(lit_str(c, t->u.prop.name)),
                 mid, t->line);
            return dst;
        }
        if (t->tag == A_STATICGET && tstr_val(t->u.scall.member)[0] == '$') {
            /* C::$prop++ : fetch, RMW in place, write back via ASSIGN_SPROP */
            TStr *ccls = t->u.scall.cls;
            const char *raw = tstr_val(ccls);
            bool is_sp = tstr_case_eq(raw, "self") || tstr_case_eq(raw, "parent") ||
                         tstr_case_eq(raw, "static");
            if (!is_sp) ccls = resolve_class_name(ccls);
            const char *cn = tstr_val(ccls);
            int kind = tstr_case_eq(cn, "self") ? 0 :
                       tstr_case_eq(cn, "parent") ? 1 :
                       tstr_case_eq(cn, "static") ? 2 : 3;
            char key[256];
            if (kind == 3)
                snprintf(key, sizeof key, "%s::%s", cn, tstr_val(t->u.scall.member));
            else
                snprintf(key, sizeof key, "%s", tstr_val(t->u.scall.member));
            Loc mid = LOC_TMP(tmp_new(c));
            emit(c, OP_FETCH_CCONST, (Loc){ .kind = OPD_UNUSED, .slot = kind },
                 LOC_CONST(lit_str(c, tstr_intern(key, strlen(key)))), mid, t->line);
            Loc dst = tmp_dst(c, want);
            emit(c, is_inc ? OP_INC : OP_DEC, mid, LOC_NONE, dst, t->line);
            c->fn->ops[c->fn->n_ops - 1].ext = prefix ? 1 : 0;
            emit(c, OP_ASSIGN_SPROP, (Loc){ .kind = OPD_UNUSED, .slot = kind },
                 LOC_CONST(lit_str(c, tstr_intern(key, strlen(key)))), mid, t->line);
            return dst;
        }
        tphp_fatal_src(c->file, t->line, "Cannot increment/decrement this expression");
    }
    case A_ISSET: {
        Loc dst = tmp_dst(c, want);
        AExprList *args = e->u.args;
        int *jz = tm_malloc(sizeof(int) * (size_t)(args->n ? args->n : 1));
        for (int i = 0; i < args->n; i++) {
            AExpr *a = args->items[i];
            Loc r;
            if (a->tag == A_VAR) {
                r = LOC_TMP(tmp_new(c));
                if (var_is_global(c, a->u.name)) {
                    emit(c, OP_ISSET_GLOBAL, LOC_CONST(lit_str(c, a->u.name)), LOC_NONE, r, a->line);
                } else {
                    int slot = get_var(c, a->u.name);
                    emit(c, OP_ISSET_CV, LOC_CV(slot), LOC_NONE, r, a->line);
                }
            } else if (a->tag == A_DIM) {
                r = compile_dim_isset(c, a);
            } else if (a->tag == A_PROP) {
                Loc obj = compile_expr(c, a->u.prop.obj, LOC_NONE);
                r = LOC_TMP(tmp_new(c));
                emit(c, OP_ISSET_PROP, obj, LOC_CONST(lit_str(c, a->u.prop.name)),
                     r, a->line);
            } else {
                tphp_fatal_src(c->file, a->line, "isset() requires a variable");
            }
            jz[i] = emit(c, OP_JMPZ, r, LOC_NONE, LOC_NONE, a->line);
        }
        emit(c, OP_CONST, LOC_CONST(lit_bool(c, true)), LOC_NONE, dst, e->line);
        int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
        uint32_t lfalse = (uint32_t)cur_pc(c);
        emit(c, OP_CONST, LOC_CONST(lit_bool(c, false)), LOC_NONE, dst, e->line);
        patch_jump(c, jend, (uint32_t)cur_pc(c));
        for (int i = 0; i < args->n; i++)
            patch_jump(c, jz[i], lfalse);
        tm_free(jz);
        return dst;
    }
    case A_EMPTY: {
        Loc dst = tmp_dst(c, want);
        AExpr *a = e->u.args->items[0];
        Loc r;
        if (a->tag == A_VAR) {
            r = compile_var_read_silent(c, a->u.name, a->line);
        } else if (a->tag == A_DIM) {
            r = compile_dim_read(c, a, true, LOC_NONE);
        } else {
            /* PHP: empty(expr) == !expr for any expression */
            r = compile_expr(c, a, LOC_NONE);
        }
        int tb = tmp_new(c);
        emit(c, OP_BOOL_VAL, r, LOC_NONE, LOC_TMP(tb), a->line);
        emit(c, OP_BOOL_NOT, LOC_TMP(tb), LOC_NONE, dst, e->line);
        return dst;
    }
    case A_MATCH: {
        Loc dst = tmp_dst(c, want);
        Loc cond = compile_expr(c, e->u.match.cond, LOC_NONE);
        AMatchList *arms = e->u.match.arms;
        int *jmp_ends = tm_malloc(sizeof(int) * (size_t)(arms->n ? arms->n : 1));
        int n_ends = 0;
        bool has_default = false;
        for (int i = 0; i < arms->n; i++) {
            AMatchArm *arm = &arms->arms[i];
            if (arm->is_default) { has_default = true; continue; }
            /* per-cond jump-into-body targets */
            int *jns = tm_malloc(sizeof(int) * (size_t)(arm->n_conds ? arm->n_conds : 1));
            for (int k = 0; k < arm->n_conds; k++) {
                Loc cv = compile_expr(c, arm->conds[k], LOC_NONE);
                int tr = tmp_new(c);
                emit(c, OP_SEQ, cond, cv, LOC_TMP(tr), arm->conds[k]->line);
                jns[k] = emit(c, OP_JMPNZ, LOC_TMP(tr), LOC_NONE, LOC_NONE, arm->conds[k]->line);
            }
            int jskip = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, arm->body->line);
            uint32_t body_start = (uint32_t)cur_pc(c);
            compile_expr(c, arm->body, dst);
            jmp_ends[n_ends++] = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, arm->body->line);
            uint32_t arm_end = (uint32_t)cur_pc(c);
            patch_jump(c, jskip, arm_end);           /* no cond matched */
            for (int k = 0; k < arm->n_conds; k++)
                patch_jump(c, jns[k], body_start);   /* cond matched -> body */
            tm_free(jns);
        }
        if (has_default) {
            /* compile the default arm body last (match arms are unordered) */
            for (int i = 0; i < arms->n; i++)
                if (arms->arms[i].is_default)
                    compile_expr(c, arms->arms[i].body, dst);
        } else {
            /* runtime UnhandledMatchError (PHP semantics) */
            int ci = tphp_ex_class_index("UnhandledMatchError");
            if (ci < 0) ci = 0;
            emit(c, OP_NEW_EX, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
            c->fn->ops[c->fn->n_ops - 1].a1 = (uint32_t)ci;
            c->fn->ops[c->fn->n_ops - 1].a2 = 0;
            emit(c, OP_THROW, LOC_TMP(tmp_new(c)), LOC_NONE, LOC_NONE, e->line);
        }
        for (int i = 0; i < n_ends; i++)
            patch_jump(c, jmp_ends[i], (uint32_t)cur_pc(c));
        tm_free(jmp_ends);
        return dst;
    }
    case A_CALL:
        return compile_call(c, e, want);
    case A_ASSIGN:
        return compile_assign(c, e, want);
    case A_ASSIGN_OP:
        return compile_assign_op(c, e, want);
    case A_CLOSURE:
    case A_ARROW:
        return compile_closure_expr(c, e, want);
    case A_NEW: {
        /* user classes resolve at runtime through the registry (allows
         * forward references and `new $cls`-style dynamics later); the
         * built-in exception hierarchy keeps the M2 fast path */
        TStr *cname;
        if (e->u.newx.anon) {
            /* anonymous class: compile + register under a unique name at
             * the declaring site, then construct like a named class */
            AClassDecl *cd = (AClassDecl *)e->u.newx.anon;
            static int anon_seq = 0;
            char nm[160];
            snprintf(nm, sizeof nm, "class@anonymous:%s:%d$%d",
                     c->file ? c->file : "?", e->line, anon_seq++);
            cd->name = tstr_intern(nm, strlen(nm));
            cd->line = e->line;
            if (cd->parent) cd->parent = resolve_class_name(cd->parent);
            for (int k = 0; k < cd->n_ifaces; k++)
                cd->ifaces[k] = resolve_class_name(cd->ifaces[k]);
            compile_class_decl(c->prog, cd, c->file);
            cname = cd->name;
        } else {
            cname = resolve_class_name(e->u.newx.name);
        }
        char lc[256];
        { const char *s = tstr_val(cname);
          size_t k = 0; for (; s[k] && k < sizeof lc - 1; k++)
              lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
          lc[k] = 0; }
        bool builtin_ex = tphp_ex_class_index(tstr_val(cname)) >= 0;
        bool user_cls = !builtin_ex && tphp_class_lookup(lc) != NULL;
        if (!builtin_ex && !user_cls)
            tphp_fatal_src(c->file, e->line, "Class '%s' not found",
                           tstr_val(cname));
        bool has_named = false, has_spread = false;
        for (int i = 0; i < e->u.newx.args->n; i++) {
            if (e->u.newx.args->items[i].name) has_named = true;
            if (e->u.newx.args->items[i].is_spread) has_spread = true;
        }
        TMethod *ctor_m = NULL;
        if (has_named) {
            if (has_spread)
                tphp_fatal_src(c->file, e->line,
                               "Cannot combine named arguments and spread");
            if (!user_cls)
                tphp_fatal_src(c->file, e->line,
                               "Named constructor arguments require a user class");
            TClass *cls = tphp_class_lookup(lc);
            if (!cls)
                tphp_fatal_src(c->file, e->line, "Class '%s' not found",
                               tstr_val(cname));
            tphp_class_ensure_flat(cls);
            if (cls->m_ctor < 0 || !cls->methods[cls->m_ctor].fn ||
                !cls->methods[cls->m_ctor].param_names)
                tphp_fatal_src(c->file, e->line,
                               "Named constructor arguments require a compiled user constructor");
            ctor_m = &cls->methods[cls->m_ctor];
        }
        int n_ctor_args;
        if (has_named) {
            /* reorder sends to ctor parameter order; a2 = n_params is exact
             * (one SEND per slot) so op_new_user arg addressing stays valid */
            n_ctor_args = compile_named_sends(c, e->line, e->u.newx.args,
                                              ctor_m->param_names, ctor_m->n_params);
        } else {
            n_ctor_args = e->u.newx.args->n;
            for (int i = 0; i < e->u.newx.args->n; i++) {
                Loc av = compile_expr(c, e->u.newx.args->items[i].e, LOC_NONE);
                emit(c, OP_SEND_VAL, av, LOC_NONE, LOC_NONE, e->line);
            }
        }
        Loc dst = tmp_dst(c, want);
        if (user_cls) {
            emit(c, OP_NEW_USER, LOC_CONST(lit_str(c, cname)),
                 LOC_NONE, dst, e->line);
            c->fn->ops[c->fn->n_ops - 1].a2 = (uint32_t)n_ctor_args;
        } else {
            emit(c, OP_NEW_EX, LOC_NONE, LOC_NONE, dst, e->line);
            c->fn->ops[c->fn->n_ops - 1].a1 =
                (uint32_t)tphp_ex_class_index(tstr_val(cname));
            c->fn->ops[c->fn->n_ops - 1].a2 = (uint32_t)n_ctor_args;
        }
        return dst;
    }
    case A_FIRSTCLASS: {
        bool fb;
        TStr *fq = resolve_func_name(e->u.name, &fb);
        Loc dst = tmp_dst(c, want);
        int at = emit(c, OP_GET_FUNC, LOC_CONST(lit_str(c, fq)), LOC_NONE, dst, e->line);
        if (fb) c->fn->ops[at].ext = 1;
        return dst;
    }
    case A_FIRSTCLASS_DYN: {
        if (e->u.e->tag == A_PROP) {
            /* P2-A: $obj->m(...) -> bound method closure (receiver +
             * method name captured; op_init_dyn dispatches it) */
            Loc obj = compile_expr(c, e->u.e->u.prop.obj, LOC_NONE);
            Loc dst = tmp_dst(c, want);
            emit(c, OP_FCC_METHOD, obj, LOC_CONST(lit_str(c, e->u.e->u.prop.name)),
                 dst, e->line);
            return dst;
        }
        Loc sv = compile_expr(c, e->u.e, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        emit(c, OP_FCC_DYN, sv, LOC_NONE, dst, e->line);
        return dst;
    }
    case A_PROP: {
        /* runtime dispatch in op_fetch_prop: exception getters (M2 fast
         * path, name-mapped) vs general user objects (M3) */
        static TStr *s_msg, *s_code, *s_prev, *s_line;
        if (!s_msg) {
            s_msg = tstr_intern("message", 7);
            s_code = tstr_intern("code", 4);
            s_prev = tstr_intern("previous", 8);
            s_line = tstr_intern("line", 4);
        }
        TStr *name = e->u.prop.name;
        /* method-call sugar: getMessage/getCode/getPrevious/getLine */
        if (strcmp(tstr_val(name), "getMessage") == 0) name = s_msg;
        else if (strcmp(tstr_val(name), "getCode") == 0) name = s_code;
        else if (strcmp(tstr_val(name), "getPrevious") == 0) name = s_prev;
        else if (strcmp(tstr_val(name), "getLine") == 0) name = s_line;
        Loc obj = compile_expr(c, e->u.prop.obj, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        if (e->nullsafe || chain_has_nullsafe(e->u.prop.obj)) {
            /* null guard: obj null (or a deeper hop already short-circuited
             * to null) -> this hop yields null too */
            int jz = emit(c, OP_JMP_NULL, obj, LOC_NONE, LOC_NONE, e->line);
            emit(c, OP_FETCH_PROP, obj, LOC_CONST(lit_str(c, name)), dst, e->line);
            int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
            patch_jump(c, jz, (uint32_t)c->fn->n_ops);
            emit(c, OP_CONST, LOC_CONST(lit_null(c)), LOC_NONE, dst, e->line);
            patch_jump(c, jend, (uint32_t)c->fn->n_ops);
        } else {
            emit(c, OP_FETCH_PROP, obj, LOC_CONST(lit_str(c, name)), dst, e->line);
        }
        return dst;
    }
    case A_METHODCALL: {
        /* general method call: INIT_MCALL + args + DO_FCALL (M3);
         * exception getters dispatch inside op_init_mcall for M2 objects.
         * operands: a1 = receiver, ar = method-name literal, a2 = argc */
        Loc obj = compile_expr(c, e->u.mcall.obj, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        int jz = -1;
        if (e->nullsafe || chain_has_nullsafe(e->u.mcall.obj)) {
            /* receiver null -> method never runs, result is null */
            jz = emit(c, OP_JMP_NULL, obj, LOC_NONE, LOC_NONE, e->line);
        }
        int init_at = emit(c, OP_INIT_MCALL, obj, LOC_NONE,
                           LOC_CONST(lit_str(c, e->u.mcall.name)), e->line);
        int n_sent = 0;
        for (int i = 0; i < e->u.mcall.args->n; i++) {
            AArg *arg = &e->u.mcall.args->items[i];
            if (arg->name)
                tphp_fatal_src(c->file, e->line,
                               "Named args on instance methods need the receiver type at compile time (use an array callable or static:: variant)");
            if (arg->is_spread) {
                Loc sv = compile_expr(c, arg->e, LOC_NONE);
                emit(c, OP_SEND_SPREAD, sv, LOC_NONE, LOC_NONE, e->line);
            } else {
                Loc av = compile_expr(c, arg->e, LOC_NONE);
                emit(c, OP_SEND_VAL, av, LOC_NONE, LOC_NONE, e->line);
                n_sent++;
            }
        }
        /* argc rides on the INIT op — never count back from n_ops: an
         * argument expression can emit any number of ops in between
         * (e.g. $me->post($m * 2) put CONST+MUL between INIT and SEND,
         * and the old `n_ops-1-n_sent` wrote argc over the MUL) */
        c->fn->ops[init_at].a2 = (uint32_t)n_sent;
        emit(c, OP_DO_FCALL, LOC_NONE, LOC_NONE, dst, e->line);
        if (jz >= 0) {
            int jend = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, e->line);
            patch_jump(c, jz, (uint32_t)c->fn->n_ops);
            emit(c, OP_CONST, LOC_CONST(lit_null(c)), LOC_NONE, dst, e->line);
            patch_jump(c, jend, (uint32_t)c->fn->n_ops);
        }
        return dst;
    }
    case A_STATICCALL: {
        /* INIT_SMCALL operands: a1 = kind (0 self/1 parent/2 static/3 named),
         * a2 = argc; ar lit = "method" or "Cls::method" (kind 3) */
        TStr *ccls = e->u.scall.cls;
        const char *raw = tstr_val(ccls);
        bool is_sp = tstr_case_eq(raw, "self") || tstr_case_eq(raw, "parent") ||
                     tstr_case_eq(raw, "static");
        if (ccls && !is_sp) ccls = resolve_class_name(ccls);
        const char *cn = tstr_val(ccls);
        int kind = tstr_case_eq(cn, "self") ? 0 :
                   tstr_case_eq(cn, "parent") ? 1 :
                   tstr_case_eq(cn, "static") ? 2 : 3;
        char key[256];
        if (kind == 3)
            snprintf(key, sizeof key, "%s::%s", cn, tstr_val(e->u.scall.member));
        else
            snprintf(key, sizeof key, "%s", tstr_val(e->u.scall.member));
        bool has_named = false, has_spread = false;
        for (int i = 0; i < e->u.scall.args->n; i++) {
            if (e->u.scall.args->items[i].name) has_named = true;
            if (e->u.scall.args->items[i].is_spread) has_spread = true;
        }
        TMethod *nm = NULL;
        if (has_named) {
            if (has_spread)
                tphp_fatal_src(c->file, e->line,
                               "Cannot combine named arguments and spread");
            /* kind 2 (static::) is late-bound: the real method's signature
             * may differ from the declaring class — approximated with it
             * (documented deviation; rewrite-with-renamed-params is rare) */
            TClass *sig_cls = c->fn->decl_cls;
            if (kind == 3) {
                char lc2[256];
                { const char *s = cn;
                  size_t k = 0; for (; s[k] && k < sizeof lc2 - 1; k++)
                      lc2[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
                  lc2[k] = 0; }
                sig_cls = tphp_class_lookup(lc2);
            } else if (kind == 1 && sig_cls) {
                tphp_class_ensure_flat(sig_cls);
                sig_cls = sig_cls->parent;
            }
            if (!sig_cls)
                tphp_fatal_src(c->file, e->line,
                               "Unknown class for named static arguments");
            char mlc[128];
            { const char *s = tstr_val(e->u.scall.member);
              size_t k = 0; for (; s[k] && k < sizeof mlc - 1; k++)
                  mlc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
              mlc[k] = 0; }
            tphp_class_ensure_flat(sig_cls);
            nm = tphp_class_find_method(sig_cls, mlc);
            if (!nm || !nm->fn || !nm->param_names)
                tphp_fatal_src(c->file, e->line,
                               "Named arguments require a compiled user method");
        }
        int init_at = emit(c, OP_INIT_SMCALL,
                           (Loc){ .kind = OPD_UNUSED, .slot = kind },
                           LOC_NONE, LOC_CONST(lit_str(c, tstr_intern(key, strlen(key)))),
                           e->line);
        int n_sent;
        if (has_named) {
            n_sent = compile_named_sends(c, e->line, e->u.scall.args,
                                         nm->param_names, nm->n_params);
        } else {
            n_sent = 0;
            for (int i = 0; i < e->u.scall.args->n; i++) {
                AArg *arg = &e->u.scall.args->items[i];
                if (arg->is_spread) {
                    Loc sv = compile_expr(c, arg->e, LOC_NONE);
                    emit(c, OP_SEND_SPREAD, sv, LOC_NONE, LOC_NONE, e->line);
                } else {
                    Loc av = compile_expr(c, arg->e, LOC_NONE);
                    emit(c, OP_SEND_VAL, av, LOC_NONE, LOC_NONE, e->line);
                    n_sent++;
                }
            }
        }
        c->fn->ops[init_at].a2 = (uint32_t)n_sent;
        Loc dst = tmp_dst(c, want);
        emit(c, OP_DO_FCALL, LOC_NONE, LOC_NONE, dst, e->line);
        return dst;
    }
    case A_STATICGET: {
        /* o1 = kind; o2 lit = "member" or "Cls::member" (kind 3) — the VM
         * splits the pair on the last "::"; dst rides on tr/ar.
         * `::class` resolves to the FQ class-name string: compile-time for
         * a plain name, runtime for self/parent/static (FETCH_CCONST) and
         * for values (A_GETCLASS). */
        TStr *ccls = e->u.scall.cls;
        const char *raw = tstr_val(ccls);
        bool is_sp = tstr_case_eq(raw, "self") || tstr_case_eq(raw, "parent") ||
                     tstr_case_eq(raw, "static");
        if (ccls && !is_sp) ccls = resolve_class_name(ccls);
        const char *cn = tstr_val(ccls);
        int kind = tstr_case_eq(cn, "self") ? 0 :
                   tstr_case_eq(cn, "parent") ? 1 :
                   tstr_case_eq(cn, "static") ? 2 : 3;
        const char *mem = tstr_val(e->u.scall.member);
        if (strcmp(mem, "class") == 0) {
            Loc dst = tmp_dst(c, want);
            if (kind != 3)
                /* self::class / static::class / parent::class at runtime */
                emit(c, OP_FETCH_CCONST, (Loc){ .kind = OPD_UNUSED, .slot = kind },
                     LOC_CONST(lit_str(c, tstr_intern("class", 5))), dst, e->line);
            else                                /* Cls::class compile-time */
                emit(c, OP_CONST, LOC_CONST(lit_str(c, ccls)), LOC_NONE, dst, e->line);
            return dst;
        }
        char key[256];
        if (kind == 3)
            snprintf(key, sizeof key, "%s::%s", cn, mem);
        else
            snprintf(key, sizeof key, "%s", mem);
        Loc dst = tmp_dst(c, want);
        emit(c, OP_FETCH_CCONST, (Loc){ .kind = OPD_UNUSED, .slot = kind },
             LOC_CONST(lit_str(c, tstr_intern(key, strlen(key)))), dst, e->line);
        return dst;
    }
    case A_GETCLASS: {
        Loc sv = compile_expr(c, e->u.e, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        emit(c, OP_GET_CLASS, sv, LOC_NONE, dst, e->line);
        return dst;
    }
    case A_INSTANCEOF: {
        Loc l = compile_expr(c, e->u.bin.l, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        emit(c, OP_INSTANCEOF, l,
             LOC_CONST(lit_str(c, resolve_class_name(e->u.bin.r->u.name))),
             dst, e->line);
        return dst;
    }
    case A_CLONE: {
        Loc src = compile_expr(c, e->u.e, LOC_NONE);
        Loc dst = tmp_dst(c, want);
        emit(c, OP_CLONE, src, LOC_NONE, dst, e->line);
        return dst;
    }
    case A_PIPELINE: {
        AExpr *call = e->u.pipe.call;
        if (call->tag != A_CALL)
            tphp_fatal_src(c->file, e->line,
                           "Right side of |> must be a function call in M2");
        AArgList *al = call->u.call.args;
        al->items = tm_realloc(al->items, sizeof(AArg) * (size_t)(al->n + 1));
        memmove(al->items + 1, al->items, sizeof(AArg) * (size_t)al->n);
        al->items[0].e = e->u.pipe.input;
        al->items[0].name = NULL;
        al->items[0].is_spread = false;
        al->n++;
        return compile_call(c, call, want);
    }
    case A_SPREAD:
        tphp_fatal_src(c->file, e->line, "Spread is only allowed inside array literals in M1");
    default:
        tphp_fatal_src(c->file, e->line, "Internal: unhandled expression node");
    }
}

/* ============ loop patch stacks ============ */
static void loop_push(Ctx *c, bool is_switch) {
    LoopPatches *lp = tm_malloc(sizeof *lp);
    memset(lp, 0, sizeof *lp);
    lp->is_switch = is_switch;
    lp->prev = c->loops;
    c->loops = lp;
}

static void int_push(int **arr, int *n, int *cap, int v) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *arr = tm_realloc(*arr, sizeof(int) * (size_t)*cap);
    }
    (*arr)[(*n)++] = v;
}

static LoopPatches *find_break_target(Ctx *c, int64_t level, int line) {
    LoopPatches *p = c->loops;
    for (int64_t i = 1; p; p = p->prev, i++)
        if (i == level) return p;
    tphp_fatal_src(c->file, line, "Cannot break out of %lld levels", (long long)level);
}

static LoopPatches *find_cont_target(Ctx *c, int64_t level, int line) {
    LoopPatches *p = c->loops;
    for (int64_t i = 1; p; p = p->prev)
        if (!p->is_switch) {           /* continue skips switch frames */
            if (i == level) return p;
            i++;
        }
    tphp_fatal_src(c->file, line, "Cannot continue out of %lld levels", (long long)level);
}

/* patch and pop the innermost loop frame */
static void loop_pop(Ctx *c, uint32_t brk_target, uint32_t cont_target, bool has_cont) {
    LoopPatches *lp = c->loops;
    for (int i = 0; i < lp->n_breaks; i++) patch_jump(c, lp->breaks[i], brk_target);
    if (has_cont)
        for (int i = 0; i < lp->n_conts; i++) patch_jump(c, lp->conts[i], cont_target);
    c->loops = lp->prev;
    tm_free(lp->breaks);
    tm_free(lp->conts);
    tm_free(lp);
}

/* ============ destructuring ============ */
static void compile_destruct_items(Ctx *c, ADestructItem **items, int n,
                                   Loc src, int64_t *auto_idx, int line) {
    for (int i = 0; i < n; i++) {
        ADestructItem *it = items[i];
        uint32_t kl = it->has_key
            ? (it->str_key ? lit_str(c, it->str_key) : lit_int(c, it->int_key))
            : lit_int(c, (*auto_idx)++);
        if (!it->nested && !it->target)
            continue;              /* skipped slot: consumes an index only */
        Loc val = LOC_TMP(tmp_new(c));
        emit(c, OP_FETCH_DIM_R, src, LOC_CONST(kl), val, line);
        if (it->nested) {
            compile_destruct_items(c, it->nested, it->n_nested, val, auto_idx, line);
            continue;
        }
        AExpr *t = it->target;
        if (!c->is_main && var_is_global(c, t->u.name)) {
            emit(c, OP_ASSIGN_GLOBAL, val, LOC_CONST(lit_str(c, t->u.name)), LOC_NONE, line);
        } else {
            int slot = get_var(c, t->u.name);
            emit(c, OP_COPY, val, LOC_NONE, LOC_CV(slot), line);
        }
    }
}

static void compile_destruct_null(Ctx *c, ADestructItem **items, int n, int line) {
    for (int i = 0; i < n; i++) {
        ADestructItem *it = items[i];
        if (!it->nested && !it->target)
            continue;
        if (it->nested) {
            compile_destruct_null(c, it->nested, it->n_nested, line);
            continue;
        }
        AExpr *t = it->target;
        if (!c->is_main && var_is_global(c, t->u.name)) {
            emit(c, OP_ASSIGN_GLOBAL, LOC_CONST(lit_null(c)),
                 LOC_CONST(lit_str(c, t->u.name)), LOC_NONE, line);
        } else {
            int slot = get_var(c, t->u.name);
            emit(c, OP_CONST, LOC_CONST(lit_null(c)), LOC_NONE, LOC_CV(slot), line);
        }
    }
}

/* ============ statements ============ */
static void compile_stmt_list(Ctx *c, AStmtList *l);

static void compile_stmt(Ctx *c, AStmt *s) {
    switch (s->tag) {
    case S_ECHO:
        for (int i = 0; i < s->u.exprs->n; i++) {
            Loc v = compile_expr(c, s->u.exprs->items[i], LOC_NONE);
            emit(c, OP_INIT_FCALL, LOC_CONST(lit_str(c, tstr_intern("print", 5))),
                 LOC_NONE, LOC_NONE, s->line);
            c->fn->ops[c->fn->n_ops - 1].a2 = 1;
            emit(c, OP_SEND_VAL, v, LOC_NONE, LOC_NONE, s->line);
            emit(c, OP_DO_FCALL, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        }
        break;
    case S_EXPR:
        compile_expr(c, s->u.e, LOC_NONE);
        break;
    case S_IF: {
        AIfChain *ch = s->u.ifs;
        int *jends = tm_malloc(sizeof(int) * (size_t)(ch->n ? ch->n : 1));
        int n_ends = 0;
        for (int i = 0; i < ch->n; i++) {
            AIfBranch *br = &ch->branches[i];
            Loc cv = compile_expr(c, br->cond, LOC_NONE);
            int t = tmp_new(c);
            emit(c, OP_BOOL_VAL, cv, LOC_NONE, LOC_TMP(t), br->cond->line);
            int jz = emit(c, OP_JMPZ, LOC_TMP(t), LOC_NONE, LOC_NONE, s->line);
            compile_stmt_list(c, br->body);
            jends[n_ends++] = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
            patch_jump(c, jz, (uint32_t)cur_pc(c));
        }
        if (ch->else_body)
            compile_stmt_list(c, ch->else_body);
        uint32_t Lend = (uint32_t)cur_pc(c);
        for (int i = 0; i < n_ends; i++) patch_jump(c, jends[i], Lend);
        tm_free(jends);
        break;
    }
    case S_WHILE: {
        uint32_t Lstart = (uint32_t)cur_pc(c);
        loop_push(c, false);
        Loc cv = compile_expr(c, s->u.wh.cond, LOC_NONE);
        int t = tmp_new(c);
        emit(c, OP_BOOL_VAL, cv, LOC_NONE, LOC_TMP(t), s->u.wh.cond->line);
        int jz = emit(c, OP_JMPZ, LOC_TMP(t), LOC_NONE, LOC_NONE, s->line);
        compile_stmt_list(c, s->u.wh.body);
        int jback = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        patch_jump(c, jback, Lstart);
        uint32_t Lend = (uint32_t)cur_pc(c);
        patch_jump(c, jz, Lend);
        loop_pop(c, Lend, Lstart, true);
        break;
    }
    case S_DOWHILE: {
        loop_push(c, false);
        uint32_t Lbody = (uint32_t)cur_pc(c);
        compile_stmt_list(c, s->u.wh.body);
        uint32_t Lcond = (uint32_t)cur_pc(c);
        Loc cv = compile_expr(c, s->u.wh.cond, LOC_NONE);
        int t = tmp_new(c);
        emit(c, OP_BOOL_VAL, cv, LOC_NONE, LOC_TMP(t), s->u.wh.cond->line);
        int jback = emit(c, OP_JMPNZ, LOC_TMP(t), LOC_NONE, LOC_NONE, s->line);
        patch_jump(c, jback, Lbody);
        uint32_t Lend = (uint32_t)cur_pc(c);
        loop_pop(c, Lend, Lcond, true);
        break;
    }
    case S_FOR: {
        AFor *f = s->u.fors;
        for (int i = 0; i < f->init->n; i++) compile_expr(c, f->init->items[i], LOC_NONE);
        uint32_t Lstart = (uint32_t)cur_pc(c);
        loop_push(c, false);
        int jz = -1;
        if (f->cond) {
            Loc cv = compile_expr(c, f->cond, LOC_NONE);
            int t = tmp_new(c);
            emit(c, OP_BOOL_VAL, cv, LOC_NONE, LOC_TMP(t), f->cond->line);
            jz = emit(c, OP_JMPZ, LOC_TMP(t), LOC_NONE, LOC_NONE, s->line);
        }
        compile_stmt_list(c, f->body);
        uint32_t Lcont = (uint32_t)cur_pc(c);
        for (int i = 0; i < f->step->n; i++) compile_expr(c, f->step->items[i], LOC_NONE);
        int jback = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        patch_jump(c, jback, Lstart);
        uint32_t Lend = (uint32_t)cur_pc(c);
        if (jz >= 0) patch_jump(c, jz, Lend);
        loop_pop(c, Lend, Lcont, true);
        break;
    }
    case S_FOREACH: {
        AForeach *fe = s->u.fe;
        Loc arr = compile_expr(c, fe->arr, LOC_NONE);
        Loc itmp = LOC_TMP(tmp_new(c));
        emit(c, OP_FE_RESET, arr, LOC_NONE, itmp, s->line);
        Loc ctmp = LOC_TMP(tmp_new(c));
        emit(c, OP_CONST, LOC_CONST(lit_int(c, 0)), LOC_NONE, ctmp, s->line);
        tmp_new(c); tmp_new(c);  /* reserve val (ctmp+1) and key (ctmp+2) */
        uint32_t Lstart = (uint32_t)cur_pc(c);
        loop_push(c, false);
        Loc htmp = LOC_TMP(tmp_new(c));
        emit(c, OP_FE_FETCH, itmp, ctmp, htmp, s->line);
        int jz = emit(c, OP_JMPZ, htmp, LOC_NONE, LOC_NONE, s->line);
        int vslot = get_var(c, fe->val_name);
        emit(c, OP_COPY, LOC_TMP(ctmp.slot + 1), LOC_NONE, LOC_CV(vslot), s->line);
        if (fe->has_key) {
            int kslot = get_var(c, fe->key_name);
            emit(c, OP_COPY, LOC_TMP(ctmp.slot + 2), LOC_NONE, LOC_CV(kslot), s->line);
        }
        compile_stmt_list(c, fe->body);
        int jback = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        patch_jump(c, jback, Lstart);
        uint32_t Lend = (uint32_t)cur_pc(c);
        patch_jump(c, jz, Lend);
        loop_pop(c, Lend, Lstart, true);
        break;
    }
    case S_SWITCH: {
        ASwitch *sw = s->u.sw;
        Loc cv = compile_expr(c, sw->cond, LOC_NONE);
        loop_push(c, true);
        int *jbody = tm_malloc(sizeof(int) * (size_t)(sw->n ? sw->n : 1));
        int n_default = -1;
        for (int i = 0; i < sw->n; i++) {
            if (sw->cases[i].is_default) { n_default = i; continue; }
            Loc cvi = compile_expr(c, sw->cases[i].cond, LOC_NONE);
            int t = tmp_new(c);
            emit(c, OP_EQ, cv, cvi, LOC_TMP(t), sw->cases[i].cond->line);
            jbody[i] = emit(c, OP_JMPNZ, LOC_TMP(t), LOC_NONE, LOC_NONE, s->line);
        }
        int j_def = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        int *body_pc = tm_malloc(sizeof(int) * (size_t)(sw->n ? sw->n : 1));
        for (int i = 0; i < sw->n; i++) {
            body_pc[i] = cur_pc(c);
            compile_stmt_list(c, sw->cases[i].body);
        }
        uint32_t Lend = (uint32_t)cur_pc(c);
        for (int i = 0; i < sw->n; i++)
            if (!sw->cases[i].is_default)
                patch_jump(c, jbody[i], (uint32_t)body_pc[i]);
        patch_jump(c, j_def, n_default >= 0 ? (uint32_t)body_pc[n_default] : Lend);
        loop_pop(c, Lend, 0, false);
        tm_free(jbody);
        tm_free(body_pc);
        break;
    }
    case S_BREAK: {
        LoopPatches *t = find_break_target(c, s->u.level, s->line);
        int j = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        int_push(&t->breaks, &t->n_breaks, &t->cap_breaks, j);
        break;
    }
    case S_CONTINUE: {
        LoopPatches *t = find_cont_target(c, s->u.level, s->line);
        int j = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        int_push(&t->conts, &t->n_conts, &t->cap_conts, j);
        break;
    }
    case S_RETURN:
        if (c->fin && c->fin->no_return)
            tphp_fatal_src(c->file, s->line,
                "return inside catch combined with finally lands in M3");
        if (c->fin) {
            /* return inside a try body: stash value, jump to a per-return
             * copy of the finally block which then RETURNs the value */
            int vt = tmp_new(c);
            if (s->u.e) {
                Loc v = compile_expr(c, s->u.e, LOC_NONE);
                emit(c, OP_COPY, v, LOC_NONE, LOC_TMP(vt), s->line);
            } else {
                emit(c, OP_CONST, LOC_CONST(lit_null(c)), LOC_NONE, LOC_TMP(vt), s->line);
            }
            FinCtx *f = c->fin;
            if (f->n_pend == f->cap_pend) {
                f->cap_pend = f->cap_pend ? f->cap_pend * 2 : 4;
                f->pend = tm_realloc(f->pend, sizeof(*f->pend) * (size_t)f->cap_pend);
            }
            f->pend[f->n_pend].jmp_pc = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
            f->pend[f->n_pend].val_tmp = vt;
            f->n_pend++;
            break;
        }
        if (s->u.e) {
            Loc v = compile_expr(c, s->u.e, LOC_NONE);
            emit(c, OP_RETURN, v, LOC_NONE, LOC_NONE, s->line);
        } else {
            emit(c, OP_RETURN, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        }
        break;
    case S_GLOBAL:
        if (!c->is_main)
            for (int i = 0; i < s->u.names.n; i++)
                mark_global_var(c, s->u.names.names[i]);
        break;
    case S_UNSET:
        for (int i = 0; i < s->u.exprs->n; i++) {
            AExpr *a = s->u.exprs->items[i];
            if (a->tag == A_VAR) {
                if (var_is_global(c, a->u.name)) {
                    /* PHP: unset($g) inside a function only breaks the
                     * local alias; the global itself survives. M1 keeps
                     * the alias, so this compiles to nothing. */
                } else {
                    int slot = get_var(c, a->u.name);
                    emit(c, OP_UNSET_CV, LOC_CV(slot), LOC_NONE, LOC_NONE, a->line);
                }
            } else if (a->tag == A_DIM) {
                WBList *wb = NULL;
                Loc base = compile_lvalue_base(c, a->u.dim.base, &wb, a->line);
                Loc idx = LOC_NONE;
                if (a->u.dim.idx)
                    idx = compile_expr(c, a->u.dim.idx, LOC_NONE);
                emit(c, OP_UNSET_DIM, base, idx, LOC_NONE, a->line);
                emit_writebacks(c, wb, a->line);
            } else if (a->tag == A_PROP) {
                /* unset($obj->prop): declared slot -> UNDEF, else __unset */
                Loc obj = compile_expr(c, a->u.prop.obj, LOC_NONE);
                emit(c, OP_UNSET_PROP, obj,
                     LOC_CONST(lit_str(c, a->u.prop.name)), LOC_NONE, a->line);
            } else {
                tphp_fatal_src(c->file, a->line, "unset() requires a variable");
            }
        }
        break;
    case S_DESTRUCT: {
        ADestructList *dl = s->u.des.dl;
        Loc rv = compile_expr(c, s->u.des.rhs, LOC_NONE);
        int j_null = emit(c, OP_JMP_NULL, rv, LOC_NONE, LOC_NONE, s->line);
        int64_t auto_idx = 0;
        compile_destruct_items(c, dl->items, dl->n, rv, &auto_idx, s->line);
        int j_end = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        patch_jump(c, j_null, (uint32_t)cur_pc(c));
        compile_destruct_null(c, dl->items, dl->n, s->line);
        patch_jump(c, j_end, (uint32_t)cur_pc(c));
        break;
    }
    case S_BLOCK:
        compile_stmt_list(c, s->u.stmts);
        break;
    case S_CONSTDECL:
        for (int i = 0; i < s->u.consts.n; i++) {
            TStr *nm = s->u.consts.decls[i].name;
            const char *raw = tstr_val(nm);
            if (raw[0] != '\\' && s_cur_ns && tstr_len(s_cur_ns))
                nm = ns_concat(s_cur_ns, nm);   /* const K -> A\B\K */
            Loc v = compile_expr(c, s->u.consts.decls[i].value, LOC_NONE);
            emit(c, OP_DEFINE_CONST, LOC_CONST(lit_str(c, nm)),
                 v, LOC_NONE, s->line);
        }
        break;
    case S_TRY: {
        ATry *tr = s->u.trys;
        uint32_t start = (uint32_t)cur_pc(c);
        FinCtx fin;
        memset(&fin, 0, sizeof fin);
        fin.prev = c->fin;
        bool has_fin = tr->finally != NULL;
        if (has_fin) c->fin = &fin;
        compile_stmt_list(c, tr->body);
        uint32_t end = (uint32_t)cur_pc(c);
        int *catch_jmps = tm_malloc(sizeof(int) * (size_t)(tr->n_catches ? tr->n_catches : 1));
        int n_cjmp = 0;

        if (!has_fin) {
            /* plain try/catch */
            int j_end1 = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
            for (int i = 0; i < tr->n_catches; i++) {
                ACatch *ct = tr->catches[i];
                uint32_t handler_pc = (uint32_t)cur_pc(c);
                int slot = -1;
                if (ct->var) {
                    slot = get_var(c, ct->var);
                    emit(c, OP_BIND_EX, LOC_NONE, LOC_NONE, LOC_CV(slot), s->line);
                }
                compile_stmt_list(c, ct->body);
                catch_jmps[n_cjmp++] = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
                TFunc *fn = c->fn;
                fn->ex = tm_realloc(fn->ex, sizeof(TExEntry) * (size_t)(fn->n_ex + 1));
                TExEntry *ent = &fn->ex[fn->n_ex++];
                /* end+1：把 try 体后的边界 JMP 纳入保护区。DO_FCALL 压帧前
                 * 会把调用方 pc 前推到边界（vm.c op_do_fcall），若调用是 try
                 * 体最后一条 op，pc == end 恰好落在排他区间外，跨帧 catch
                 * 会整段错过 handler（2026-10-15 审计 P0-1）。JMP 自身不会
                 * raise，纳入无副作用；嵌套区间的相对次序不变。 */
                ent->start_pc = start; ent->end_pc = end + 1; ent->handler_pc = handler_pc;
                uint16_t *cls = tm_malloc(sizeof(uint16_t) * (size_t)ct->n_cls);
                const TStr **nm = tm_malloc(sizeof(TStr *) * (size_t)ct->n_cls);
                for (int k = 0; k < ct->n_cls; k++) {
                    TStr *cn = resolve_class_name(ct->cls[k]);
                    /* P1-2: 内置异常类（TExClass 索引快路）+ 用户异常类
                     * （TClass 名字路径，exclass.c 注册的内部类也走名字） */
                    int ci = tphp_ex_class_index(tstr_val(cn));
                    cls[k] = (uint16_t)(ci >= 0 ? ci : 0xFFFF);
                    nm[k] = cn;
                }
                ent->catch_cls = cls;
                ent->catch_nm = nm;
                ent->n_catches = (uint8_t)ct->n_cls;
                ent->has_var = ct->var ? 1 : 0;
                ent->catch_slot = slot >= 0 ? (uint16_t)slot : 0;
            }
            uint32_t Lend = (uint32_t)cur_pc(c);
            patch_jump(c, j_end1, Lend);
            for (int i = 0; i < n_cjmp; i++) patch_jump(c, catch_jmps[i], Lend);
            tm_free(catch_jmps);
            break;
        }

        /* try/catch/finally. Returns inside the try body run a per-return
         * copy of the finally block before RETURNing (pend trampolines).
         * A return inside a catch body combined with finally is rejected
         * at load time (rare pattern; full fast_call scheme lands in M3). */
        int *pend_jmps = tm_malloc(sizeof(int) * (size_t)(fin.n_pend ? fin.n_pend : 1));
        /* normal-path finally copy + end-of-block exit */
        uint32_t normal_entry = (uint32_t)cur_pc(c);
        (void)normal_entry;
        compile_stmt_list(c, tr->finally);            /* normal-path copy */
        int j_norm_end = -1;
        if (c->is_main) {
            j_norm_end = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        } else {
            /* body completed without return: implicit `return null` */
            int nt = tmp_new(c);
            emit(c, OP_CONST, LOC_CONST(lit_null(c)), LOC_NONE, LOC_TMP(nt), s->line);
            emit(c, OP_RETURN, LOC_TMP(nt), LOC_NONE, LOC_NONE, s->line);
        }
        /* per-return trampolines: copy of finally + RETURN(val_tmp) */
        for (int p = 0; p < fin.n_pend; p++)
            pend_jmps[p] = -1;
        uint32_t *pentry = tm_malloc(sizeof(uint32_t) * (size_t)(fin.n_pend ? fin.n_pend : 1));
        for (int p = 0; p < fin.n_pend; p++) {
            pentry[p] = (uint32_t)cur_pc(c);
            compile_stmt_list(c, tr->finally);        /* copy #p */
            emit(c, OP_RETURN, LOC_TMP(fin.pend[p].val_tmp), LOC_NONE, LOC_NONE, s->line);
        }
        int j_end1 = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        for (int i = 0; i < tr->n_catches; i++) {
            ACatch *ct = tr->catches[i];
            uint32_t handler_pc = (uint32_t)cur_pc(c);
            int slot = -1;
            if (ct->var) {
                slot = get_var(c, ct->var);
                emit(c, OP_BIND_EX, LOC_NONE, LOC_NONE, LOC_CV(slot), s->line);
            }
            if (has_fin) c->fin->no_return = true;
            compile_stmt_list(c, ct->body);
            if (has_fin) c->fin->no_return = false;
            if (fin.n_pend || true)
                compile_stmt_list(c, tr->finally);    /* catch-path copy */
            catch_jmps[n_cjmp++] = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
            TFunc *fn = c->fn;
            fn->ex = tm_realloc(fn->ex, sizeof(TExEntry) * (size_t)(fn->n_ex + 1));
            TExEntry *ent = &fn->ex[fn->n_ex++];
            /* end+1 同 !has_fin 路径：边界纳入保护区（P0-1，见上注释） */
            ent->start_pc = start; ent->end_pc = end + 1; ent->handler_pc = handler_pc;
            uint16_t *cls = tm_malloc(sizeof(uint16_t) * (size_t)ct->n_cls);
            const TStr **nm = tm_malloc(sizeof(TStr *) * (size_t)ct->n_cls);
            for (int k = 0; k < ct->n_cls; k++) {
                TStr *cn = resolve_class_name(ct->cls[k]);
                int ci = tphp_ex_class_index(tstr_val(cn));
                cls[k] = (uint16_t)(ci >= 0 ? ci : 0xFFFF);
                nm[k] = cn;
            }
            ent->catch_cls = cls;
            ent->catch_nm = nm;
            ent->n_catches = (uint8_t)ct->n_cls;
            ent->has_var = ct->var ? 1 : 0;
            ent->catch_slot = slot >= 0 ? (uint16_t)slot : 0;
        }
        if (tr->finally) {
            uint32_t fin3 = (uint32_t)cur_pc(c);
            compile_stmt_list(c, tr->finally);        /* exception-path copy */
            emit(c, OP_RETHROW, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
            TFunc *fn = c->fn;
            fn->ex = tm_realloc(fn->ex, sizeof(TExEntry) * (size_t)(fn->n_ex + 1));
            TExEntry *ent = &fn->ex[fn->n_ex++];
            ent->start_pc = start; ent->end_pc = end + 1; ent->handler_pc = fin3;
            int ci = tphp_ex_class_index("Throwable");
            uint16_t *cls = tm_malloc(sizeof(uint16_t));
            cls[0] = (uint16_t)(ci >= 0 ? ci : 0xFFFF);
            ent->catch_cls = cls;
            const TStr **nm = tm_malloc(sizeof(TStr *));
            nm[0] = tstr_intern("Throwable", 9);
            ent->catch_nm = nm;
            ent->n_catches = 1;
            ent->has_var = 0;
            ent->catch_slot = 0;
        }
        uint32_t Lend = (uint32_t)cur_pc(c);
        if (c->is_main)
            patch_jump(c, j_norm_end, Lend);
        patch_jump(c, j_end1, Lend);
        for (int i = 0; i < n_cjmp; i++) patch_jump(c, catch_jmps[i], Lend);
        for (int p = 0; p < fin.n_pend; p++)
            patch_jump(c, fin.pend[p].jmp_pc, pentry[p]);
        tm_free(catch_jmps);
        tm_free(pend_jmps);
        tm_free(pentry);
        if (fin.pend) tm_free(fin.pend);
        c->fin = fin.prev;
        break;
    }
    case S_CLASSDECL:
        /* registered during pass 1.5; nothing to emit at this site */
        break;
    case S_NS_BEGIN:
        ns_enter(s->u.name);
        break;
    case S_NS_END:
        ns_enter(s->u.name);      /* carries the outer namespace */
        break;
    case S_USE:
        /* apply at the compile position: the use table is reset before the
         * main walk, so imports must be (re)applied here in statement order
         * to affect only the code that follows them (PHP use semantics) */
        apply_use_list(s->u.use);
        break;
    case S_THROW: {
        Loc v = compile_expr(c, s->u.e, LOC_NONE);
        emit(c, OP_THROW, v, LOC_NONE, LOC_NONE, s->line);
        break;
    }
    case S_GOTO: {
        int j = emit(c, OP_JMP, LOC_NONE, LOC_NONE, LOC_NONE, s->line);
        if (c->n_gotos == 0) {
            c->gotos = tm_malloc(sizeof(*c->gotos) * 8);
        } else if (c->n_gotos % 8 == 0) {
            c->gotos = tm_realloc(c->gotos, sizeof(*c->gotos) * (size_t)(c->n_gotos + 8));
        }
        c->gotos[c->n_gotos].label = s->u.name;
        c->gotos[c->n_gotos].jmp_pc = j;
        c->n_gotos++;
        break;
    }
    case S_LABEL:
        if (c->n_labels == 0) {
            c->labels = tm_malloc(sizeof(*c->labels) * 8);
        } else if (c->n_labels % 8 == 0) {
            c->labels = tm_realloc(c->labels, sizeof(*c->labels) * (size_t)(c->n_labels + 8));
        }
        c->labels[c->n_labels].label = s->u.name;
        c->labels[c->n_labels].pc = cur_pc(c);
        c->n_labels++;
        break;
    case S_FUNCDECL:   /* hoisted by tphp_compile */
    case S_NOP:
        break;
    }
}

static void compile_stmt_list(Ctx *c, AStmtList *l) {
    for (int i = 0; i < l->n; i++)
        compile_stmt(c, l->stmts[i]);
}

/* ============ program assembly ============ */
static void scan_globals_stmt(AStmt *s, TStr ***out, int *n, int *cap);

static void scan_globals_list(AStmtList *l, TStr ***out, int *n, int *cap) {
    for (int i = 0; i < l->n; i++)
        scan_globals_stmt(l->stmts[i], out, n, cap);
}

static void scan_globals_stmt(AStmt *s, TStr ***out, int *n, int *cap) {
    switch (s->tag) {
    case S_GLOBAL:
        for (int i = 0; i < s->u.names.n; i++) {
            if (*n == *cap) {
                *cap = *cap ? *cap * 2 : 8;
                *out = tm_realloc(*out, sizeof(TStr *) * (size_t)*cap);
            }
            (*out)[(*n)++] = s->u.names.names[i];
        }
        break;
    case S_IF:
        for (int i = 0; i < s->u.ifs->n; i++)
            scan_globals_list(s->u.ifs->branches[i].body, out, n, cap);
        if (s->u.ifs->else_body)
            scan_globals_list(s->u.ifs->else_body, out, n, cap);
        break;
    case S_WHILE: case S_DOWHILE:
        scan_globals_list(s->u.wh.body, out, n, cap);
        break;
    case S_FOR:
        scan_globals_list(s->u.fors->body, out, n, cap);
        break;
    case S_FOREACH:
        scan_globals_list(s->u.fe->body, out, n, cap);
        break;
    case S_SWITCH:
        for (int i = 0; i < s->u.sw->n; i++)
            scan_globals_list(s->u.sw->cases[i].body, out, n, cap);
        break;
    case S_BLOCK:
        scan_globals_list(s->u.stmts, out, n, cap);
        break;
    case S_FUNCDECL:
        scan_globals_list(s->u.func->body, out, n, cap);
        break;
    default:
        break;
    }
}

/* ---- M2: closures, default-value thunks, goto fixups ---- */

static uint16_t compile_thunk(Program *p, AExpr *expr, const char *file);

static int g_closure_counter = 0;

static void collect_expr_vars(AExpr *e, AExprList *out, AParam **params, int n_params);
static void collect_stmt_vars(AStmt *s, AExprList *out, AParam **params, int n_params);

static bool is_param(AParam **params, int n_params, TStr *name) {
    for (int i = 0; i < n_params; i++)
        if (params[i]->name == name) return true;
    return false;
}

static void add_use(AExprList *out, TStr *name, int line) {
    for (int i = 0; i < out->n; i++)
        if (out->items[i]->u.name == name) return;
    AExpr *v = tm_malloc(sizeof(AExpr));
    memset(v, 0, sizeof *v);
    v->tag = A_VAR;
    v->line = line;
    v->u.name = name;
    ael_push_c(out, v);
}

static void collect_expr_vars(AExpr *e, AExprList *out, AParam **params, int n_params) {
    if (!e) return;
    switch (e->tag) {
    case A_VAR:
        if (!is_param(params, n_params, e->u.name)) add_use(out, e->u.name, e->line);
        return;
    case A_INT: case A_FLOAT: case A_STR:
    case A_TRUE: case A_FALSE: case A_NULL: case A_CONSTREF:
    case A_FIRSTCLASS:
        return;
    case A_FIRSTCLASS_DYN:
        collect_expr_vars(e->u.e, out, params, n_params);
        return;
    case A_ARRAY: {
        AArrayList *al = e->u.alist;
        for (int i = 0; i < al->n; i++) {
            if (al->items[i].key) collect_expr_vars(al->items[i].key, out, params, n_params);
            collect_expr_vars(al->items[i].val, out, params, n_params);
        }
        return;
    }
    case A_BINARY:
        collect_expr_vars(e->u.bin.l, out, params, n_params);
        collect_expr_vars(e->u.bin.r, out, params, n_params);
        return;
    case A_UNARY: case A_INCDEC:
        collect_expr_vars(e->u.un.e, out, params, n_params);
        return;
    case A_ASSIGN: case A_ASSIGN_OP:
        collect_expr_vars(e->u.bin.l, out, params, n_params);
        collect_expr_vars(e->u.bin.r, out, params, n_params);
        return;
    case A_TERNARY: case A_COALESCE:
        collect_expr_vars(e->u.tern.c, out, params, n_params);
        if (e->u.tern.m) collect_expr_vars(e->u.tern.m, out, params, n_params);
        collect_expr_vars(e->u.tern.e, out, params, n_params);
        return;
    case A_CALL:
        if (e->u.call.dyn) collect_expr_vars(e->u.call.dyn, out, params, n_params);
        if (e->u.call.args)
            for (int i = 0; i < e->u.call.args->n; i++)
                collect_expr_vars(e->u.call.args->items[i].e, out, params, n_params);
        return;
    case A_DIM:
        collect_expr_vars(e->u.dim.base, out, params, n_params);
        if (e->u.dim.idx) collect_expr_vars(e->u.dim.idx, out, params, n_params);
        return;
    case A_CAST:
        collect_expr_vars(e->u.cast.e, out, params, n_params);
        return;
    case A_MATCH: {
        collect_expr_vars(e->u.match.cond, out, params, n_params);
        for (int i = 0; i < e->u.match.arms->n; i++) {
            AMatchArm *arm = &e->u.match.arms->arms[i];
            for (int k = 0; k < arm->n_conds; k++)
                collect_expr_vars(arm->conds[k], out, params, n_params);
            collect_expr_vars(arm->body, out, params, n_params);
        }
        return;
    }
    case A_ISSET: case A_EMPTY:
        for (int i = 0; i < e->u.args->n; i++)
            collect_expr_vars(e->u.args->items[i], out, params, n_params);
        return;
    case A_SPREAD:
        collect_expr_vars(e->u.e, out, params, n_params);
        return;
    case A_CLOSURE: case A_ARROW:
        return;
    case A_NEW:
        for (int i = 0; i < e->u.newx.args->n; i++)
            collect_expr_vars(e->u.newx.args->items[i].e, out, params, n_params);
        return;
    case A_PIPELINE:
        collect_expr_vars(e->u.pipe.input, out, params, n_params);
        collect_expr_vars(e->u.pipe.call, out, params, n_params);
        return;

    case A_PROP:
        collect_expr_vars(e->u.prop.obj, out, params, n_params);
        return;
    case A_METHODCALL:
        collect_expr_vars(e->u.mcall.obj, out, params, n_params);
        return;
    case A_STATICCALL: {
        for (int i = 0; i < e->u.scall.args->n; i++)
            collect_expr_vars(e->u.scall.args->items[i].e, out, params, n_params);
        return;
    }
    case A_INSTANCEOF:
        collect_expr_vars(e->u.bin.l, out, params, n_params);
        return;
    case A_CLONE:
        collect_expr_vars(e->u.e, out, params, n_params);
        return;
    }
}

static void collect_stmt_vars(AStmt *s, AExprList *out, AParam **params, int n_params) {
    if (!s) return;
    switch (s->tag) {
    case S_ECHO:
        for (int i = 0; i < s->u.exprs->n; i++)
            collect_expr_vars(s->u.exprs->items[i], out, params, n_params);
        break;
    case S_EXPR:
        collect_expr_vars(s->u.e, out, params, n_params);
        break;
    case S_IF:
        for (int i = 0; i < s->u.ifs->n; i++) {
            collect_expr_vars(s->u.ifs->branches[i].cond, out, params, n_params);
            for (int j = 0; j < s->u.ifs->branches[i].body->n; j++)
                collect_stmt_vars(s->u.ifs->branches[i].body->stmts[j], out, params, n_params);
        }
        if (s->u.ifs->else_body)
            for (int j = 0; j < s->u.ifs->else_body->n; j++)
                collect_stmt_vars(s->u.ifs->else_body->stmts[j], out, params, n_params);
        break;
    case S_WHILE: case S_DOWHILE:
        collect_expr_vars(s->u.wh.cond, out, params, n_params);
        for (int j = 0; j < s->u.wh.body->n; j++)
            collect_stmt_vars(s->u.wh.body->stmts[j], out, params, n_params);
        break;
    case S_FOR:
        for (int i = 0; i < s->u.fors->init->n; i++)
            collect_expr_vars(s->u.fors->init->items[i], out, params, n_params);
        if (s->u.fors->cond) collect_expr_vars(s->u.fors->cond, out, params, n_params);
        for (int i = 0; i < s->u.fors->step->n; i++)
            collect_expr_vars(s->u.fors->step->items[i], out, params, n_params);
        for (int j = 0; j < s->u.fors->body->n; j++)
            collect_stmt_vars(s->u.fors->body->stmts[j], out, params, n_params);
        break;
    case S_FOREACH:
        collect_expr_vars(s->u.fe->arr, out, params, n_params);
        for (int j = 0; j < s->u.fe->body->n; j++)
            collect_stmt_vars(s->u.fe->body->stmts[j], out, params, n_params);
        break;
    case S_SWITCH:
        collect_expr_vars(s->u.sw->cond, out, params, n_params);
        for (int i = 0; i < s->u.sw->n; i++) {
            if (s->u.sw->cases[i].cond)
                collect_expr_vars(s->u.sw->cases[i].cond, out, params, n_params);
            for (int j = 0; j < s->u.sw->cases[i].body->n; j++)
                collect_stmt_vars(s->u.sw->cases[i].body->stmts[j], out, params, n_params);
        }
        break;
    case S_RETURN:
        collect_expr_vars(s->u.e, out, params, n_params);
        break;
    case S_UNSET:
        for (int i = 0; i < s->u.exprs->n; i++)
            collect_expr_vars(s->u.exprs->items[i], out, params, n_params);
        break;
    case S_CLASSDECL:
        break;
    case S_THROW:
        collect_expr_vars(s->u.e, out, params, n_params);
        break;
    case S_DESTRUCT:
        collect_expr_vars(s->u.des.rhs, out, params, n_params);
        break;
    case S_BLOCK:
        for (int j = 0; j < s->u.stmts->n; j++)
            collect_stmt_vars(s->u.stmts->stmts[j], out, params, n_params);
        break;
    case S_TRY:
        for (int j = 0; j < s->u.trys->body->n; j++)
            collect_stmt_vars(s->u.trys->body->stmts[j], out, params, n_params);
        for (int i = 0; i < s->u.trys->n_catches; i++)
            for (int j = 0; j < s->u.trys->catches[i]->body->n; j++)
                collect_stmt_vars(s->u.trys->catches[i]->body->stmts[j], out, params, n_params);
        if (s->u.trys->finally)
            for (int j = 0; j < s->u.trys->finally->n; j++)
                collect_stmt_vars(s->u.trys->finally->stmts[j], out, params, n_params);
        break;
    case S_CONSTDECL:
        for (int i = 0; i < s->u.consts.n; i++)
            collect_expr_vars(s->u.consts.decls[i].value, out, params, n_params);
        break;
    default: break;
    }
}

/* compile one function-like body (named fn, closure, arrow, thunk, method) */
static uint16_t compile_fn_like(Program *p, AFunc *af, Ctx *parent,
                                bool is_closure, bool auto_capture,
                                bool is_method, void *decl_cls,
                                const char *file) {
    TFunc *f = tm_malloc(sizeof *f);
    memset(f, 0, sizeof *f);
    f->name = af->name;
    f->ret_type = af->ret_type;   /* P1-1 */
    f->is_closure = is_closure;
    f->is_method = is_method;
    f->is_variadic = af->n_params > 0 && af->params[af->n_params - 1]->is_variadic;
    f->decl_cls = decl_cls;
    f->n_params = (uint16_t)af->n_params;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.file = file;
    c.fn = f;
    c.is_main = false;
    c.prog = p;
    c.global_names = parent ? parent->global_names : NULL;
    c.n_global_names = parent ? parent->n_global_names : 0;
    c.sigs = parent ? parent->sigs : NULL;
    c.n_sigs = parent ? parent->n_sigs : 0;
    if (is_method)
        get_var(&c, tstr_intern("this", 4));   /* CV slot 0 = $this */
    uint16_t n_cap = 0;
    AExprList *uses = af->uses;
    if (auto_capture) {
        uses = ael_new_c();
        for (int j = 0; j < af->body->n; j++)
            collect_stmt_vars(af->body->stmts[j], uses, af->params, af->n_params);
    }
    /* P1-3: 方法内创建的闭包自动捕获 $this（static 闭包除外，对齐 PHP）。
     * compile_closure_expr 侧以同一顺序重算该列表做 SEND_CAP——两处都靠
     * add_use 的指针去重保持幂等/一致。 */
    if (is_closure && decl_cls && !af->is_static) {
        if (!uses) uses = ael_new_c();
        add_use(uses, tstr_intern("this", 4), af->line);
    }
    if (uses) {
        for (int i = 0; i < uses->n; i++) {
            get_var(&c, uses->items[i]->u.name);
            n_cap++;
        }
    }
    f->n_cap = n_cap;
    if (af->n_params) {
        f->defaults = tm_malloc(sizeof(TVal) * (size_t)af->n_params);
        f->default_thunk = tm_malloc(sizeof(uint16_t) * (size_t)af->n_params);
        f->param_types = tm_malloc(sizeof(TStr *) * (size_t)af->n_params);
        for (int i = 0; i < af->n_params; i++) {
            get_var(&c, af->params[i]->name);
            f->default_thunk[i] = 0;
            f->param_types[i] = af->params[i]->type;   /* P1-1 */
            if (af->params[i]->default_val) {
                bool ok = true;
                TVal d = eval_const2(&c, af->params[i]->default_val, &ok);
                if (ok) {
                    f->defaults[i] = d;
                } else {
                    f->defaults[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
                    f->default_thunk[i] = compile_thunk(p, af->params[i]->default_val, file);
                }
            } else {
                f->defaults[i] = (TVal){ .u.i = 0, .t = T_UNDEF }; /* required */
            }
        }
    }
    for (int i = 0; i < af->n_params; i++) {
        if (af->params[i]->is_variadic)
            continue;   /* the variadic slot is filled by RECV_VARIADIC */
        int pslot = get_var(&c, af->params[i]->name);   /* after capture slots */
        emit(&c, OP_RECV, (Loc){ .kind = OPD_UNUSED, .slot = i }, LOC_NONE, LOC_CV(pslot), af->line);
    }
    if (af->n_params && af->params[af->n_params - 1]->is_variadic) {
        int vslot = get_var(&c, af->params[af->n_params - 1]->name);
        emit(&c, OP_RECV_VARIADIC, (Loc){ .kind = OPD_UNUSED, .slot = af->n_params - 1 },
             LOC_NONE, LOC_CV(vslot), af->line);
    }
    compile_stmt_list(&c, af->body);
    emit(&c, OP_RETURN, LOC_NONE, LOC_NONE, LOC_NONE, af->line); /* implicit */
    resolve_gotos(&c, file);
    ctx_finish(&c);
    p->funcs = tm_realloc(p->funcs, sizeof(TFunc *) * (size_t)(p->n_funcs + 1));
    p->funcs[p->n_funcs++] = f;
    return (uint16_t)(p->n_funcs - 1);
}

static Loc compile_closure_expr(Ctx *c, AExpr *e, Loc want) {
    AFunc *af = e->u.clo.func;
    bool auto_capture = e->tag == A_ARROW;
    AExprList *uses = af->uses;
    if (auto_capture) {
        uses = ael_new_c();
        for (int j = 0; j < af->body->n; j++)
            collect_stmt_vars(af->body->stmts[j], uses, af->params, af->n_params);
    }
    /* P1-3: 方法内创建的闭包自动捕获 $this（static 闭包除外，对齐 PHP），
     * 闭包 TFunc 继承声明类作用域——private/protected 访问与 self:: 解析
     * 随闭包生效。捕获源是外层方法帧的 CV 0，SEND_CAP 按名取。 */
    TClass *scope = c->fn->decl_cls;
    if (scope && !af->is_static) {
        if (!uses) uses = ael_new_c();
        add_use(uses, tstr_intern("this", 4), e->line);
    }
    uint16_t idx = compile_fn_like(c->prog, af, c, true, auto_capture, false,
                                   scope, c->file);
    if (uses) {
        for (int i = 0; i < uses->n; i++) {
            TStr *name = uses->items[i]->u.name;
            Loc v;
            if (!c->is_main && var_is_global(c, name)) {
                v = LOC_TMP(tmp_new(c));
                emit(c, OP_FETCH_GLOBAL, LOC_NONE, LOC_CONST(lit_str(c, name)), v, e->line);
            } else {
                v = LOC_CV(get_var(c, name));
            }
            emit(c, OP_SEND_CAP, v, LOC_NONE, LOC_NONE, e->line);
        }
    }
    Loc dst = tmp_dst(c, want);
    emit(c, OP_CLOSURE_INIT, (Loc){ .kind = OPD_UNUSED, .slot = idx },
         LOC_NONE, dst, e->line);
    c->fn->ops[c->fn->n_ops - 1].a2 = uses ? (uint32_t)uses->n : 0;
    return dst;
}

/* zero-arg function evaluating one expression (runtime default values) */
static uint16_t compile_thunk(Program *p, AExpr *expr, const char *file) {
    AFunc af;
    memset(&af, 0, sizeof af);
    char nm[32];
    snprintf(nm, sizeof nm, "~thunk%d", ++g_closure_counter);
    af.name = tstr_intern(nm, strlen(nm));
    AStmtList *l = asl_new_c();
    AStmt *rs = as_new_c(S_RETURN, expr->line);
    rs->u.e = expr;                     /* thunk returns the expression value */
    asl_push_c(l, rs);
    af.body = l;
    af.line = expr->line;
    return compile_fn_like(p, &af, NULL, false, false, false, NULL, file);
}

static void resolve_gotos(Ctx *c, const char *file) {
    for (int i = 0; i < c->n_gotos; i++) {
        int target = -1;
        for (int j = 0; j < c->n_labels; j++)
            if (c->labels[j].label == c->gotos[i].label) { target = c->labels[j].pc; break; }
        if (target < 0)
            tphp_fatal_src(file, 0, "Undefined label in goto");
        patch_jump(c, c->gotos[i].jmp_pc, (uint32_t)target);
    }
}

/* ---- class declarations (M3) ----
 * Builds the TClass with own members, registers it. Flattening (inheritance,
 * traits) is lazy in class.c. Namespaces land later in M3: names are used
 * as-is (single-namespace scripts until the namespace pass). */
static void compile_class_decl(Program *p, AClassDecl *cd, const char *file) {
    TClass *cls = tm_malloc(sizeof(TClass));
    memset(cls, 0, sizeof *cls);
    cls->name = cd->name;
    cls->kind = (ClassKind)cd->kind;
    cls->flags = cd->flags;
    cls->parent_name = cd->parent;
    for (int i = 0; i < cd->n_ifaces; i++) {
        cls->iface_names = tm_realloc(cls->iface_names,
            sizeof(TStr *) * (size_t)(cls->n_iface_names + 1));
        cls->iface_names[cls->n_iface_names++] = cd->ifaces[i];
    }
    if (cd->kind == CK_ENUM && cd->backing_marker) {
        const char *bm = tstr_val(cd->backing_marker);
        if (tstr_case_eq(bm, "int")) { cls->backed = true; cls->backing_type = T_INT; }
        else if (tstr_case_eq(bm, "string")) { cls->backed = true; cls->backing_type = T_STR; }
        else tphp_fatal_src(file, cd->line, "Enum backing type must be int or string");
    }

    for (int i = 0; i < cd->body->n; i++) {
        AClassMember *mem = cd->body->items[i];
        switch (mem->tag) {
        case CM_METHOD: {
            AFunc *af = mem->func;
            char nl[64];
            { const char *s = tstr_val(af->name);
              size_t k = 0; for (; s[k] && k < sizeof nl - 1; k++)
                  nl[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
              nl[k] = 0; }
            bool is_ctor = strcmp(nl, "__construct") == 0;
            /* abstract / interface methods have no body: register the
             * signature only, no TFunc is compiled (fn stays NULL) */
            if (!af->body) {
                TMethod m;
                memset(&m, 0, sizeof m);
                m.name = af->name;
                m.visibility = mem->vis;
                m.is_static = mem->is_static;
                m.is_abstract = true;
                m.is_final = mem->is_final;
                m.n_params = (uint16_t)af->n_params;
                if (af->n_params) {
                    m.param_names = tm_malloc(sizeof(TStr *) * (size_t)af->n_params);
                    for (int pi = 0; pi < af->n_params; pi++)
                        m.param_names[pi] = af->params[pi]->name;
                }
                cls->methods = tm_realloc(cls->methods, sizeof(TMethod) * (size_t)(cls->n_methods + 1));
                cls->methods[cls->n_methods++] = m;
                break;
            }
            /* constructor property promotion: register the properties on
             * the class and inject `$this->p = $p;` at the ctor top */
            if (is_ctor) {
                for (int pi = 0; pi < af->n_params; pi++) {
                    AParam *par = af->params[pi];
                    if (!par->is_promoted) continue;
                    TProp prop;
                    memset(&prop, 0, sizeof prop);
                    prop.name = par->name;
                    prop.visibility = (uint8_t)par->vis;
                    prop.is_readonly = par->is_readonly;
                    prop.defval = (TVal){ .u.i = 0, .t = T_UNDEF };
                    cls->props = tm_realloc(cls->props,
                        sizeof(TProp) * (size_t)(cls->n_props + 1));
                    cls->props[cls->n_props++] = prop;
                    /* inject AST: $this-><name> = <param>; */
                    AExpr *tv = ae_new_c(A_VAR, mem->line);
                    tv->u.name = tstr_intern("this", 4);
                    AExpr *pr = ae_new_c(A_PROP, mem->line);
                    pr->u.prop.obj = tv;
                    pr->u.prop.name = par->name;
                    AExpr *pv = ae_new_c(A_VAR, mem->line);
                    pv->u.name = par->name;
                    AExpr *as = ae_new_c(A_ASSIGN, mem->line);
                    as->u.bin.l = pr;
                    as->u.bin.r = pv;
                    AStmt *st = as_new_c(S_EXPR, mem->line);
                    st->u.e = as;
                    AStmtList *nb = asl_new_c();
                    asl_push_c(nb, st);
                    for (int k = 0; k < af->body->n; k++)
                        asl_push_c(nb, af->body->stmts[k]);
                    af->body = nb;
                }
            }
            uint16_t idx = compile_fn_like(p, af, NULL, false, false, true, cls, file);
            TMethod m;
            memset(&m, 0, sizeof m);
            m.name = af->name;
            m.fn = p->funcs[idx];
            m.visibility = mem->vis;
            m.is_static = mem->is_static;
            m.is_abstract = mem->is_abstract;
            m.is_final = mem->is_final;
            m.n_params = (uint16_t)af->n_params;
            if (af->n_params) {
                m.param_names = tm_malloc(sizeof(TStr *) * (size_t)af->n_params);
                for (int pi = 0; pi < af->n_params; pi++)
                    m.param_names[pi] = af->params[pi]->name;
            }
            cls->methods = tm_realloc(cls->methods, sizeof(TMethod) * (size_t)(cls->n_methods + 1));
            cls->methods[cls->n_methods++] = m;
            break;
        }
        case CM_PROP: {
            TProp prop;
            memset(&prop, 0, sizeof prop);
            prop.name = mem->prop.name;
            prop.type = mem->prop.type;   /* P1-1: 运行期赋值校验 */
            prop.visibility = mem->vis;
            prop.is_static = mem->is_static;
            prop.is_readonly = mem->is_readonly;
            prop.defval = (TVal){ .u.i = 0, .t = T_UNDEF };
            /* PHP：readonly 属性必须有类型 */
            if (prop.is_readonly && !prop.type)
                tphp_fatal_src(file, mem->line,
                               "Readonly property %s::$%s must have type",
                               cd->name ? tstr_val(cd->name) : "?",
                               tstr_val(mem->prop.name));
            if (mem->prop.def) {
                Ctx dc; memset(&dc, 0, sizeof dc);
                dc.file = file;
                bool ok = true;
                TVal d = eval_const2(&dc, mem->prop.def, &ok);
                if (ok) {
                    prop.defval = d;
                    /* PHP：默认值在声明期按类型校验（常量默认值部分） */
                    if (prop.type) {
                        const char *given = NULL;
                        if (!tphp_type_accepts(tstr_val(prop.type), d, NULL, &given))
                            tphp_fatal_src(file, mem->line,
                                           "Default value for property %s::$%s of type %s cannot be %s",
                                           cd->name ? tstr_val(cd->name) : "?",
                                           tstr_val(mem->prop.name),
                                           tstr_val(prop.type),
                                           given ? given : "?");
                    }
                }
                else prop.def_thunk = compile_thunk(p, mem->prop.def, file);
            }
            if (prop.is_static) {
                cls->sprops = tm_realloc(cls->sprops, sizeof(TProp) * (size_t)(cls->n_sprops + 1));
                cls->sprops[cls->n_sprops++] = prop;
            } else {
                cls->props = tm_realloc(cls->props, sizeof(TProp) * (size_t)(cls->n_props + 1));
                cls->props[cls->n_props++] = prop;
            }
            break;
        }
        case CM_CONST: {
            Ctx dc; memset(&dc, 0, sizeof dc);
            dc.file = file;
            bool ok = true;
            TVal d = eval_const2(&dc, mem->prop.def, &ok);
            if (!ok)
                tphp_fatal_src(file, mem->line, "Class constant expressions must be constant");
            cls->cnames = tm_realloc(cls->cnames, sizeof(TStr *) * (size_t)(cls->n_consts + 1));
            cls->cvals = tm_realloc(cls->cvals, sizeof(TVal) * (size_t)(cls->n_consts + 1));
            cls->cnames[cls->n_consts] = mem->prop.name;
            cls->cvals[cls->n_consts] = d;
            tval_addref(d);
            cls->n_consts++;
            break;
        }
        case CM_CASE: {
            if (cd->kind != CK_ENUM)
                tphp_fatal_src(file, mem->line, "'case' is only valid inside enums");
            TEnumCase ec;
            memset(&ec, 0, sizeof ec);
            ec.name = mem->prop.name;
            ec.backing = (TVal){ .u.i = 0, .t = T_NULL };
            if (mem->prop.def) {
                Ctx dc; memset(&dc, 0, sizeof dc);
                dc.file = file;
                bool ok = true;
                ec.backing = eval_const2(&dc, mem->prop.def, &ok);
                if (!ok)
                    tphp_fatal_src(file, mem->line, "Enum case values must be constant");
            } else if (cls->backed) {
                tphp_fatal_src(file, mem->line, "Backed enum case needs a value");
            }
            if (cls->backed) {
                if (cls->backing_type == T_INT && ec.backing.t != T_INT)
                    tphp_fatal_src(file, mem->line, "Enum case value must be int");
                if (cls->backing_type == T_STR && ec.backing.t != T_STR)
                    tphp_fatal_src(file, mem->line, "Enum case value must be string");
            }
            cls->cases = tm_realloc(cls->cases, sizeof(TEnumCase) * (size_t)(cls->n_cases + 1));
            cls->cases[cls->n_cases++] = ec;
            break;
        }
        case CM_USE: {
            for (int k = 0; k < mem->use.n; k++) {
                cls->trait_names = tm_realloc(cls->trait_names,
                    sizeof(TStr *) * (size_t)(cls->n_trait_names + 1));
                cls->trait_names[cls->n_trait_names++] =
                    resolve_class_name(mem->use.names[k]);
            }
            if (mem->use.mods)                  /* qualify adaptation refs */
                for (int k = 0; k < mem->use.mods->n; k++) {
                    AMod *md = &mem->use.mods->items[k];
                    if (md->trait) md->trait = resolve_class_name(md->trait);
                    for (int j = 0; j < md->n_from; j++)
                        md->from[j] = resolve_class_name(md->from[j]);
                }
            cls->trait_mods = mem->use.mods;   /* as/insteadof, applied in class.c */
            break;
        }
        }
    }
    if (!tphp_class_register_compile(cls))
        tphp_fatal_src(file, cd->line, "Cannot redeclare class %s",
                       tstr_val(cls->name));
}

/* apply a file-level use statement to the shared import table */
static void apply_use_list(AUseList *ul) {
    UseEnt **tab; int *n, *cap;
    if (ul->kind == 0)      { tab = &s_use.cls;   n = &s_use.n_cls;   cap = &s_use.cap_cls; }
    else if (ul->kind == 1) { tab = &s_use.func;  n = &s_use.n_func;  cap = &s_use.cap_func; }
    else                    { tab = &s_use.konst; n = &s_use.n_konst; cap = &s_use.cap_konst; }
    for (int i = 0; i < ul->n; i++) {
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 8;
            *tab = tm_realloc(*tab, sizeof(UseEnt) * (size_t)*cap);
        }
        TStr *nm = ul->items[i].name;
        const char *s = tstr_val(nm);
        (*tab)[*n].alias = ul->items[i].alias;
        (*tab)[*n].fq = s[0] == '\\'
            ? tstr_intern(s + 1, tstr_len(nm) - 1) : nm;
        (*n)++;
    }
}

/* NS-aware statement walks; declarations inside namespace blocks unfold
 * like top-level ones (plain blocks recurse too, matching hoisting) */
static void pass_sigs(AStmtList *l, FnSig **sigs, int *n_sigs, int *cap_sigs) {
    for (int i = 0; i < l->n; i++) {
        AStmt *st = l->stmts[i];
        if (st->tag == S_NS_BEGIN) { s_cur_ns = st->u.name; continue; }
        if (st->tag == S_NS_END)   { s_cur_ns = st->u.name; continue; }
        if (st->tag == S_USE)      { apply_use_list(st->u.use); continue; }
        if (st->tag == S_BLOCK)    { pass_sigs(st->u.stmts, sigs, n_sigs, cap_sigs); continue; }
        if (st->tag != S_FUNCDECL) continue;
        AFunc *af = st->u.func;
        bool fb;
        TStr *fq = resolve_func_name(af->name, &fb);
        if (*n_sigs == *cap_sigs) {
            *cap_sigs = *cap_sigs ? *cap_sigs * 2 : 8;
            *sigs = tm_realloc(*sigs, sizeof(FnSig) * (size_t)*cap_sigs);
        }
        (*sigs)[*n_sigs].name = fq;
        (*sigs)[*n_sigs].params = NULL;
        (*sigs)[*n_sigs].n_params = af->n_params;
        if (af->n_params) {
            (*sigs)[*n_sigs].params =
                tm_malloc(sizeof(TStr *) * (size_t)af->n_params);
            for (int k = 0; k < af->n_params; k++)
                (*sigs)[*n_sigs].params[k] = af->params[k]->name;
        }
        (*n_sigs)++;
    }
}

static void pass_classes(AStmtList *l, Program *p, const char *file) {
    for (int i = 0; i < l->n; i++) {
        AStmt *st = l->stmts[i];
        if (st->tag == S_NS_BEGIN) { s_cur_ns = st->u.name; continue; }
        if (st->tag == S_NS_END)   { s_cur_ns = st->u.name; continue; }
        if (st->tag == S_USE)      { apply_use_list(st->u.use); continue; }
        if (st->tag == S_BLOCK)    { pass_classes(st->u.stmts, p, file); continue; }
        if (st->tag != S_CLASSDECL) continue;
        AClassDecl *cd = st->u.cls;
        if (s_cur_ns && tstr_len(s_cur_ns))
            cd->name = ns_concat(s_cur_ns, cd->name);
        if (cd->parent) cd->parent = resolve_class_name(cd->parent);
        for (int k = 0; k < cd->n_ifaces; k++)
            cd->ifaces[k] = resolve_class_name(cd->ifaces[k]);
        compile_class_decl(p, cd, file);
    }
}

static void pass_funcs(AStmtList *l, Program *p, const char *file) {
    for (int i = 0; i < l->n; i++) {
        AStmt *st = l->stmts[i];
        if (st->tag == S_NS_BEGIN) { s_cur_ns = st->u.name; continue; }
        if (st->tag == S_NS_END)   { s_cur_ns = st->u.name; continue; }
        if (st->tag == S_USE)      { apply_use_list(st->u.use); continue; }
        if (st->tag == S_BLOCK)    { pass_funcs(st->u.stmts, p, file); continue; }
        if (st->tag != S_FUNCDECL) continue;
        AFunc *af = st->u.func;
        af->name = resolve_decl_name(af->name);
        compile_fn_like(p, af, NULL, false, false, false, NULL, file);
    }
}

/* ---- M6: require/include 编译期内联 ----
 * S_INCLUDE 在编译入口展开：读文件 → parse → 语句列表原地替换。
 * 路径相对当前文件目录；全部 require/include 按路径去重（防循环）。
 * 非字面量路径 parse 阶段已拒绝。 */

static char **s_inc_seen;
static int s_n_inc_seen, s_cap_inc_seen;

static char *dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *s = slash > bslash ? slash : bslash;
    if (!s) return NULL;
    size_t n = (size_t)(s - path);
    char *d = tm_malloc(n + 1);
    memcpy(d, path, n);
    d[n] = 0;
    return d;
}

static char *join_path(const char *dir, const char *rel) {
    /* Windows 路径：用 '\\' 或 '/' 都可，统一 '/' */
    if (!dir) {
        char *r = tm_malloc(strlen(rel) + 1);
        strcpy(r, rel);
        return r;
    }
    size_t dl = strlen(dir), rl = strlen(rel);
    char *r = tm_malloc(dl + 1 + rl + 1);
    memcpy(r, dir, dl);
    r[dl] = '/';
    memcpy(r + dl + 1, rel, rl + 1);
    return r;
}

static bool inc_seen(const char *path) {
    for (int i = 0; i < s_n_inc_seen; i++)
        if (strcmp(s_inc_seen[i], path) == 0) return true;
    return false;
}

static void inc_mark(const char *path) {
    if (s_n_inc_seen >= s_cap_inc_seen) {
        s_cap_inc_seen = s_cap_inc_seen ? s_cap_inc_seen * 2 : 16;
        s_inc_seen = tm_realloc(s_inc_seen, sizeof(char *) * (size_t)s_cap_inc_seen);
    }
    s_inc_seen[s_n_inc_seen] = tm_malloc(strlen(path) + 1);
    strcpy(s_inc_seen[s_n_inc_seen], path);
    s_n_inc_seen++;
}

static char *read_file_all(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }
    char *buf = tm_malloc((size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

/* 递归展开 stmts 中的 S_INCLUDE（顶层 + 控制流块内） */
static void expand_includes(AStmtList *stmts, const char *cur_file);

static void expand_list(AStmtList *l, const char *cur_file) {
    if (!l) return;
    expand_includes(l, cur_file);
}

static void expand_includes(AStmtList *stmts, const char *cur_file) {
    if (!stmts || stmts->n == 0) return;
    for (int i = 0; i < stmts->n; i++) {
        AStmt *st = stmts->stmts[i];
        switch (st->tag) {
        case S_BLOCK: expand_list(st->u.stmts, cur_file); break;
        case S_IF:
            if (st->u.ifs) {
                for (int b = 0; b < st->u.ifs->n; b++)
                    expand_list(st->u.ifs->branches[b].body, cur_file);
                expand_list(st->u.ifs->else_body, cur_file);
            }
            break;
        case S_WHILE: case S_DOWHILE: expand_list(st->u.wh.body, cur_file); break;
        case S_FOR: expand_list(st->u.fors->body, cur_file); break;
        case S_FOREACH: expand_list(st->u.fe->body, cur_file); break;
        case S_SWITCH:
            if (st->u.sw)
                for (int c = 0; c < st->u.sw->n; c++)
                    expand_list(st->u.sw->cases[c].body, cur_file);
            break;
        case S_TRY:
            if (st->u.trys) {
                expand_list(st->u.trys->body, cur_file);
                for (int c = 0; c < st->u.trys->n_catches; c++)
                    expand_list(st->u.trys->catches[c]->body, cur_file);
                expand_list(st->u.trys->finally, cur_file);
            }
            break;
        default: break;
        }
    }
    /* 展开顶层 S_INCLUDE：重建语句数组 */
    int n_out = 0;
    AStmt **out = NULL;
    for (int i = 0; i < stmts->n; i++) {
        AStmt *st = stmts->stmts[i];
        if (st->tag != S_INCLUDE) {
            out = tm_realloc(out, sizeof(AStmt *) * (size_t)(n_out + 1));
            out[n_out++] = st;
            continue;
        }
        const char *rel = tstr_val(st->u.inc.path);
        /* M6: 先查内嵌 bundle 表（单 exe）；命中用内嵌源码 */
        char bname[128];
        tphp_bundle_name_of(rel, bname, sizeof bname);
        const TBundle *bun = tphp_bundle_find(bname);
        if (bun) {
            if (inc_seen(bname)) continue;
            inc_mark(bname);
            AStmtList *inc = tphp_parse_script(bun->src, bun->name);
            expand_includes(inc, bun->name);
            for (int k = 0; k < inc->n; k++) {
                out = tm_realloc(out, sizeof(AStmt *) * (size_t)(n_out + 1));
                out[n_out++] = inc->stmts[k];
            }
            tm_free(inc->stmts);
            tm_free(inc);
            continue;
        }
        char *dir = cur_file ? dir_of(cur_file) : NULL;
        char *full = join_path(dir, rel);
        if (dir) tm_free(dir);
        /* 全部按路径去重（防循环 require；PHP 普通 require 可重复，
         * M6 先去重更安全——lib 函数定义场景无影响） */
        if (inc_seen(full)) { tm_free(full); continue; }
        char *src = read_file_all(full, NULL);
        if (!src)
            tphp_fatal_src(cur_file ? cur_file : "<main>", st->line,
                           "Failed opening required file '%s'", full);
        inc_mark(full);
        AStmtList *inc = tphp_parse_script(src, full);
        expand_includes(inc, full);
        tm_free(src);
        tm_free(full);
        for (int k = 0; k < inc->n; k++) {
            out = tm_realloc(out, sizeof(AStmt *) * (size_t)(n_out + 1));
            out[n_out++] = inc->stmts[k];
        }
        tm_free(inc->stmts);
        tm_free(inc);
    }
    if (out) {
        stmts->stmts = out;
        stmts->n = n_out;
    }
}

Program *tphp_compile(AStmtList *ast, const char *file) {
    /* M6: 展开 require/include（字符串字面量） */
    expand_includes(ast, file);
    Program *p = tm_malloc(sizeof *p);
    memset(p, 0, sizeof *p);
    p->file = file;
    /* reserve funcs[0]: function index 0 means "no thunk" in default_thunk
     * slots, so every real function must live at index >= 1 */
    p->funcs = tm_malloc(sizeof(TFunc *));
    p->funcs[0] = tm_malloc(sizeof(TFunc));
    memset(p->funcs[0], 0, sizeof(TFunc));
    p->n_funcs = 1;
    ns_reset();
    /* pass 1: collect top-level function signatures (named-arg reorder);
     * namespace/use statements are applied to the shared compile state */
    FnSig *sigs = NULL;
    int n_sigs = 0, cap_sigs = 0;
    pass_sigs(ast, &sigs, &n_sigs, &cap_sigs);
    ns_reset();
    /* global-alias prescan */
    TStr **gn = NULL;
    int n_gn = 0, cap_gn = 0;
    scan_globals_list(ast, &gn, &n_gn, &cap_gn);
    /* pass 1.5: compile & register class declarations (top level, in order;
     * names resolve case-insensitively and forward references work at
     * runtime through the registry) */
    pass_classes(ast, p, file);
    ns_reset();
    TFunc *m = tm_malloc(sizeof *m);
    memset(m, 0, sizeof *m);
    m->name = tstr_intern("~main", 5);
    m->is_main = true;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.file = file;
    c.fn = m;
    c.is_main = true;
    c.prog = p;
    c.global_names = gn;
    c.n_global_names = n_gn;
    c.sigs = sigs;
    c.n_sigs = n_sigs;
    for (int i = 0; i < n_gn; i++)
        get_var(&c, gn[i]);          /* reserve global-alias slots up front */
    compile_stmt_list(&c, ast);
    emit(&c, OP_RETURN, LOC_NONE, LOC_NONE, LOC_NONE, 0);
    resolve_gotos(&c, file);
    ctx_finish(&c);
    p->main = m;
    /* pass 2: hoisted top-level functions (NS-aware) */
    pass_funcs(ast, p, file);
    ns_reset();
    tm_free(gn);
    for (int i = 0; i < n_sigs; i++)
        if (sigs[i].params) tm_free(sigs[i].params);
    tm_free(sigs);
    return p;
}