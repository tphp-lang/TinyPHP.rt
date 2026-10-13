/*
 * TinyPHP.rt - a small PHP runtime
 * vm.c - CALL-dispatch interpreter: handler table, frames, call protocol,
 *        exceptions, closures
 *
 * Frames: [captures 0..n_cap) + [CV n_cap..n_locals) + [TMP ..+n_tmps) TVal
 * slots. CVs start as T_UNDEF; reads through rdv() raise UndefinedError.
 *
 * Call protocol:
 *   INIT_FCALL/INIT_DYN push a CallRec (args_base = current arg-stack top,
 *   resolution memoized in the opcode's inline cache)
 *   SEND_VAL / SEND_SPREAD push TVals onto the arg stack (addref)
 *   DO_FCALL builtin: runs, releases args, writes dst, returns 1
 *   DO_FCALL user/closure: releases oversent args, pops the arg stack,
 *     switches to a fresh frame (T_UNDEF slots; closure capture slots are
 *     pre-filled with addref'd copies) and returns 0 — the CallRec stays on
 *     the stack until RETURN writes the value into the caller's dst
 *   RECV (first n_params instructions of every function) moves the argument
 *     out of the arg stack into its CV, or applies the default / thunk
 *   RETURN: stores the value into the caller slot, releases the frame
 *
 * Exceptions: per-function exception table (pc ranges). vm_raise() stores
 * the exception object and longjmps to the main loop, which runs
 * vm_unwind(): the innermost table entry covering the current pc wins; a
 * matching catch jumps into the handler (BIND_EX binds the variable),
 * otherwise the frame is destroyed (stack trace collected) and the
 * unwinder walks to the caller. Reaching the bottom prints an Uncaught
 * error in PHP format and exits 255.
 *
 * COW: every handler that mutates an array slot calls tarr_cow() and swaps
 * the slot contents, so assignments share arrays until a write happens.
 */
#include "opcode.h"
#include "tphp.h"
#include "class.h"
#include "tsched.h"
#include "tffi.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

/* ---- builtins (builtin.c) ---- */


/* ---- built-in exception class table (shared with compile.c) ---- */
static const TExClass EX_THROWABLE = { "Throwable", NULL };
static const TExClass EX_ERROR = { "Error", &EX_THROWABLE };
static const TExClass EX_TYPE = { "TypeError", &EX_ERROR };
static const TExClass EX_ARITY = { "ArgumentCountError", &EX_ERROR };
static const TExClass EX_DIVZERO = { "DivisionByZeroError", &EX_ERROR };
static const TExClass EX_UNDEF = { "UndefinedError", &EX_ERROR };
static const TExClass EX_COMPILE = { "CompileError", &EX_ERROR };
static const TExClass EX_EXCEPTION = { "Exception", &EX_THROWABLE };
static const TExClass EX_RUNTIME = { "RuntimeException", &EX_EXCEPTION };
static const TExClass EX_LOGIC = { "LogicException", &EX_EXCEPTION };
static const TExClass EX_INVALIDARG = { "InvalidArgumentException", &EX_LOGIC };
static const TExClass EX_ERROREX = { "ErrorException", &EX_EXCEPTION };
static const TExClass EX_ASYNC_CANCEL = { "AsyncCancelled", &EX_THROWABLE };
static const TExClass EX_DEADLOCK = { "DeadlockError", &EX_ERROR };
static const TExClass EX_NOTCLONEABLE = { "NotCloneableError", &EX_ERROR };
static const TExClass EX_UNMATCH = { "UnhandledMatchError", &EX_ERROR };

extern const TExClass *tphp_ex_classes[];
extern const int tphp_ex_class_count;

const TExClass *tphp_ex_classes[] = {
    &EX_THROWABLE, &EX_ERROR, &EX_TYPE, &EX_ARITY, &EX_DIVZERO,
    &EX_UNDEF, &EX_COMPILE, &EX_EXCEPTION, &EX_RUNTIME, &EX_LOGIC,
    &EX_INVALIDARG, &EX_ERROREX, &EX_ASYNC_CANCEL, &EX_DEADLOCK,
    &EX_NOTCLONEABLE, &EX_UNMATCH,
};
const int tphp_ex_class_count =
    (int)(sizeof tphp_ex_classes / sizeof tphp_ex_classes[0]);

int tphp_ex_class_index(const char *name) {
    for (int i = 0; i < tphp_ex_class_count; i++)
        if (strcmp(tphp_ex_classes[i]->name, name) == 0) return i;
    return -1;
}

static const TExClass *ex_class(const char *name) {
    for (int i = 0; i < tphp_ex_class_count; i++)
        if (strcmp(tphp_ex_classes[i]->name, name) == 0)
            return tphp_ex_classes[i];
    return &EX_ERROR;
}

/* ---- object allocation (registered in the GC registry) ---- */
TStr *tstr_alloc_raw(uint32_t len, bool interned);
TStr *tstr_new(const char *s, size_t len);

static TClosure *tclo_alloc(TFunc *fn, TVal *caps, uint16_t n_cap) {
    TClosure *c = tm_malloc(sizeof *c);
    c->h.rc = 1; c->h.color = GC_COLOR_UNMARKED; c->h.kind = GC_KIND_CLO;
    c->h.flags = 0;
    gc_register(&c->h);
    c->fn = fn;
    c->bfn = NULL;
    c->n_cap = n_cap;
    c->captures = tm_malloc(sizeof(TVal) * (size_t)(n_cap ? n_cap : 1));
    for (uint16_t i = 0; i < n_cap; i++) {
        tval_addref(caps[i]);
        c->captures[i] = caps[i];
    }
    return c;
}

/* first-class wrapper around a builtin function */
static TClosure *tclo_alloc_builtin(BuiltinFn bfn) {
    TClosure *c = tm_malloc(sizeof *c);
    c->h.rc = 1; c->h.color = GC_COLOR_UNMARKED; c->h.kind = GC_KIND_CLO;
    c->h.flags = 0;
    gc_register(&c->h);
    c->fn = NULL;
    c->bfn = (void *)bfn;
    c->n_cap = 0;
    c->captures = tm_malloc(sizeof(TVal));
    return c;
}

static TExObj *tex_alloc(const TExClass *cls, TStr *msg, int64_t code, int line,
                         TExObj *previous) {
    TExObj *x = tm_malloc(sizeof *x);
    x->h.rc = 1; x->h.color = GC_COLOR_UNMARKED; x->h.kind = GC_KIND_EX;
    x->h.flags = 0;
    gc_register(&x->h);
    x->cls = cls;
    x->message = msg;
    tval_addref(TV_STR(msg));
    x->code = code;
    x->previous = previous;
    x->line = line;
    return x;
}

/* ---- engine raise: store the exception and longjmp to the main loop ---- */
static void vm_raise_at(VM *vm, const TExClass *cls, int line,
                        const char *fmt, va_list ap) {
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    TStr *msg = tstr_new(buf, strlen(buf));
    vm->x.exception = tex_alloc(cls, msg, 0, line, NULL);
    longjmp(vm->x.unwind, 1);
}

static void vm_raise(VM *vm, const TExClass *cls, TOp *op,
                     const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int line = op ? op->line : 0;
    if (!line && vm->x.n_calls > 0)
        line = vm->x.calls[vm->x.n_calls - 1].line;
    vm_raise_at(vm, cls, line, fmt, ap);
    va_end(ap);
}

/* vm.c: raise a typed exception from a builtin (exported to builtin.c) */
void tphp_vm_raise(VM *vm, const char *cls, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int line = 0;
    if (vm->x.frame && vm->x.frame->pc < vm->x.frame->fn->n_ops)
        line = vm->x.frame->fn->ops[vm->x.frame->pc].line;
    if (!line && vm->x.n_calls > 0)
        line = vm->x.calls[vm->x.n_calls - 1].line;
    vm_raise_at(vm, ex_class(cls), line, fmt, ap);
    va_end(ap);
}

/* value.c hook: convert engine value errors into exceptions */
static void vm_value_raise(const char *cls_name, const char *fmt, va_list ap) {
    VM *vm = (VM *)tphp_vm_current;
    int line = 0;
    if (vm && vm->x.frame && vm->x.frame->pc < vm->x.frame->fn->n_ops)
        line = vm->x.frame->fn->ops[vm->x.frame->pc].line;
    if (!line && vm && vm->x.n_calls > 0)
        line = vm->x.calls[vm->x.n_calls - 1].line;
    vm_raise_at(vm, ex_class(cls_name), line, fmt, ap);
}

/* internal: evaluate a zero-arg compiled thunk (runtime default values) */
TVal tphp_call_thunk(VM *vm, uint16_t fn_idx);

/* ---- operand helpers ---- */
static TVal *opd(VM *vm, TOp *op, int w) {
    VMFrame *f = vm->x.frame;
    switch (w) {
    case 1:
        if (op->t1 == OPD_CONST) return &f->fn->lits[op->a1];
        return &f->slots[op->a1];
    case 2:
        if (op->t2 == OPD_CONST) return &f->fn->lits[op->a2];
        return &f->slots[op->a2];
    default:
        if (op->tr == OPD_CONST) return &f->fn->lits[op->ar];
        return &f->slots[op->ar];
    }
}

/* raw read (T_UNDEF allowed) */
static TVal rd(VM *vm, TOp *op, int w) {
    return *opd(vm, op, w);
}

/* read with undefined-variable check */
static TVal rdv(VM *vm, TOp *op, int w) {
    TVal *p = opd(vm, op, w);
    if (p->t == T_UNDEF) {
        uint8_t k = w == 1 ? op->t1 : w == 2 ? op->t2 : op->tr;
        if (k == OPD_CV) {
            uint32_t slot = w == 1 ? op->a1 : w == 2 ? op->a2 : op->ar;
            vm_raise(vm, ex_class("UndefinedError"), op, "Undefined variable $%s",
                     tstr_val(vm->x.frame->fn->local_names[slot]));
        }
        vm_raise(vm, ex_class("Error"), op,
                 "Internal: read of undefined temporary (op %d %s, pc %u)",
                 op->op, tphp_op_name(op->op), vm->x.frame->pc);
    }
    return *p;
}

/* write dst (tr), releasing its old value; v ownership moves in.
 * tr == OPD_UNUSED discards v. */
static void wtake(VM *vm, TOp *op, TVal v) {
    if (op->tr == OPD_UNUSED) { tval_release(&v); return; }
    TVal *p = &vm->x.frame->slots[op->ar];
    tval_release(p);
    *p = v;
}

/* main frame slot lookup by name (the main frame IS the global scope) */
static TVal *global_slot(VM *vm, TStr *name) {
    TFunc *fn = vm->main_frame->fn;
    for (uint16_t i = 0; i < fn->n_locals; i++)
        if (tstr_eq(fn->local_names[i], name))
            return &vm->main_frame->slots[i];
    return NULL;
}

static void fatal_missing_key(VM *vm, TOp *op, TVal key) {
    if (key.t == T_INT)
        vm_raise(vm, ex_class("UndefinedError"), op, "Undefined array key %lld",
                 (long long)key.u.i);
    vm_raise(vm, ex_class("UndefinedError"), op, "Undefined array key \"%s\"",
             tstr_val((TStr *)key.u.p));
}

/* COW separation for a writable array slot; returns the (possibly new) TArr */
static TArr *dim_cow(VM *vm, TVal *bp) {
    TArr *a = tarr_cow((TArr *)bp->u.p);
    if (a != (TArr *)bp->u.p) {
        tval_release(bp);
        *bp = TV_ARR(a);
    }
    return a;
}

/* key of data slot di (for iteration / spread) */
static TVal arr_key_at(TArr *a, uint32_t di) {
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t != T_NULL && bk->val_off == di)
            return bk->key;
    }
    return TV_NULL;
}

/* shared core of ASSIGN_DIM / ARRAY_APPEND: slot(o1)[key(o2)] = value(tr) */
static int32_t do_dim_write(VM *vm, TOp *op) {
    TVal *bp = opd(vm, op, 1);
    /* M5.5: FFI\CData 指针/数组写（base 已是 CData 对象） */
    if (bp->t == T_OBJ) {
        TVal key = op->t2 != OPD_UNUSED ? rd(vm, op, 2) : TV_NULL;
        TVal val = rdv(vm, op, 3);
        if (tphp_ffi_assign_dim(vm, *bp, key, val))
            return 1;
    }
    if (bp->t == T_UNDEF) {
        TArr *a = tarr_new();
        *bp = TV_ARR(a);                       /* auto-vivification */
    }
    if (bp->t != T_ARR) {
        if (bp->t == T_STR)
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot use a string offset as an array in M1");
        vm_raise(vm, ex_class("TypeError"), op,
                 "Cannot use a scalar value as an array");
    }
    TArr *a = dim_cow(vm, bp);
    TVal val = rdv(vm, op, 3);
    tval_addref(val);
    if (op->t2 == OPD_UNUSED) {
        tarr_append(a, val);
    } else {
        TVal key = rd(vm, op, 2);
        if (!tval_norm_key(&key))
            vm_raise(vm, ex_class("TypeError"), op, "Illegal offset type");
        tarr_set(a, key, val);
    }
    return 1;
}

/* ---- opcode handlers ---- */
static int32_t op_const(VM *vm, TOp *op) {
    TVal v = vm->x.frame->fn->lits[op->a1];
    tval_addref(v);
    wtake(vm, op, v);
    return 1;
}

static int32_t op_copy(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    tval_addref(v);
    wtake(vm, op, v);
    return 1;
}

#define BIN_HANDLER(name, fn) \
    static int32_t name(VM *vm, TOp *op) { \
        TVal r = fn(rdv(vm, op, 1), rdv(vm, op, 2)); \
        wtake(vm, op, r); \
        return 1; \
    }

BIN_HANDLER(op_add, tval_add)
BIN_HANDLER(op_sub, tval_sub)
BIN_HANDLER(op_mul, tval_mul)
BIN_HANDLER(op_div, tval_div)
BIN_HANDLER(op_mod, tval_mod)
BIN_HANDLER(op_pow, tval_pow)
BIN_HANDLER(op_concat, tval_concat)
BIN_HANDLER(op_bw_and, tval_band)
BIN_HANDLER(op_bw_or, tval_bor)
BIN_HANDLER(op_bw_xor, tval_bxor)
BIN_HANDLER(op_shl, tval_shl)
BIN_HANDLER(op_shr, tval_shr)

static int32_t op_eq(VM *vm, TOp *op) {
    bool b = tval_loose_eq(rdv(vm, op, 1), rdv(vm, op, 2));
    wtake(vm, op, b ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_ne(VM *vm, TOp *op) {
    bool b = tval_loose_eq(rdv(vm, op, 1), rdv(vm, op, 2));
    wtake(vm, op, b ? TV_FALSE : TV_TRUE);
    return 1;
}

static int32_t op_seq(VM *vm, TOp *op) {
    bool b = tval_strict_eq(rdv(vm, op, 1), rdv(vm, op, 2));
    wtake(vm, op, b ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_sne(VM *vm, TOp *op) {
    bool b = tval_strict_eq(rdv(vm, op, 1), rdv(vm, op, 2));
    wtake(vm, op, b ? TV_FALSE : TV_TRUE);
    return 1;
}

static int32_t op_cmp(VM *vm, TOp *op) {
    int c = tval_compare(rdv(vm, op, 1), rdv(vm, op, 2));
    wtake(vm, op, TV_INT(c));
    return 1;
}

static int32_t op_lt(VM *vm, TOp *op) {
    int c = tval_compare(rdv(vm, op, 1), rdv(vm, op, 2));
    wtake(vm, op, c < 0 ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_le(VM *vm, TOp *op) {
    int c = tval_compare(rdv(vm, op, 1), rdv(vm, op, 2));
    wtake(vm, op, c <= 0 ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_bool_not(VM *vm, TOp *op) {
    wtake(vm, op, tval_bool_not(rdv(vm, op, 1)));
    return 1;
}

static int32_t op_bool_val(VM *vm, TOp *op) {
    TVal v = rd(vm, op, 1);
    bool b = v.t != T_UNDEF && tval_to_bool(v);
    wtake(vm, op, b ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_neg(VM *vm, TOp *op) {
    wtake(vm, op, tval_neg(rdv(vm, op, 1)));
    return 1;
}

static int32_t op_pos(VM *vm, TOp *op) {
    wtake(vm, op, tval_pos(rdv(vm, op, 1)));
    return 1;
}

static int32_t op_bit_not(VM *vm, TOp *op) {
    wtake(vm, op, tval_bit_not(rdv(vm, op, 1)));
    return 1;
}

static int32_t op_cast(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    int ct = (int)vm->x.frame->fn->lits[op->a2].u.i;
    switch (ct) {
    case 1:
        wtake(vm, op, TV_INT(tval_to_int(v)));
        return 1;
    case 2:
        wtake(vm, op, tval_to_bool(v) ? TV_TRUE : TV_FALSE);
        return 1;
    case 3:
        wtake(vm, op, TV_FLOAT(tval_to_float(v)));
        return 1;
    case 4:
        wtake(vm, op, TV_STR(tval_to_str(v)));
        return 1;
    default: { /* array */
        if (v.t == T_ARR) {
            tval_addref(v);
            wtake(vm, op, v);
        } else if (v.t == T_NULL) {
            wtake(vm, op, TV_ARR(tarr_new()));
        } else {
            TArr *a = tarr_new();
            tval_addref(v);
            tarr_append(a, v);
            wtake(vm, op, TV_ARR(a));
        }
        return 1;
    }
    }
}

/* ---- control flow ---- */
static int32_t op_jmp(VM *vm, TOp *op) {
    vm->x.frame->pc = op->ar;
    return 0;
}

static int32_t op_jmpz(VM *vm, TOp *op) {
    TVal v = rd(vm, op, 1);
    if (v.t == T_UNDEF || !tval_to_bool(v))
        vm->x.frame->pc = op->ar;
    else
        vm->x.frame->pc++;
    return 0;
}

static int32_t op_jmpnz(VM *vm, TOp *op) {
    TVal v = rd(vm, op, 1);
    if (v.t == T_UNDEF || !tval_to_bool(v))
        vm->x.frame->pc++;
    else
        vm->x.frame->pc = op->ar;
    return 0;
}

static int32_t op_jmp_null(VM *vm, TOp *op) {
    TVal v = rd(vm, op, 1);
    if (v.t == T_NULL || v.t == T_UNDEF)
        vm->x.frame->pc = op->ar;
    else
        vm->x.frame->pc++;
    return 0;
}

/* ---- fetch ---- */
static int32_t op_fetch_dim_r(VM *vm, TOp *op) {
    TVal base = rdv(vm, op, 1);
    if (op->t2 == OPD_UNUSED)
        vm_raise(vm, ex_class("Error"), op, "Cannot use [] for reading");
    TVal key = rd(vm, op, 2);
    /* M5.5: FFI\CData 指针/数组下标 */
    {
        TVal fv;
        if (tphp_ffi_fetch_dim(vm, base, key, &fv)) {
            wtake(vm, op, fv);
            return 1;
        }
    }
    if (base.t == T_ARR) {
        TArr *a = (TArr *)base.u.p;
        if (!tval_norm_key(&key))
            vm_raise(vm, ex_class("TypeError"), op, "Illegal offset type");
        TVal *v = tarr_get(a, key);
        if (!v) fatal_missing_key(vm, op, key);
        tval_addref(*v);
        wtake(vm, op, *v);
        return 1;
    }
    if (base.t == T_STR) {
        TStr *s = (TStr *)base.u.p;
        if (!tval_norm_key(&key) || key.t != T_INT)
            vm_raise(vm, ex_class("TypeError"), op,
                     "Cannot access offset of type %s on string",
                     key.t == T_INT ? "int" : key.t == T_STR ? "string" : "unsupported");
        int64_t i = key.u.i;
        if (i < 0) i += s->len;
        if (i < 0 || i >= (int64_t)s->len)
            vm_raise(vm, ex_class("UndefinedError"), op,
                     "Uninitialized string offset %lld", (long long)key.u.i);
        wtake(vm, op, TV_STR(tstr_new(s->c + i, 1)));
        return 1;
    }
    vm_raise(vm, ex_class("TypeError"), op,
             "Cannot access offset on %s",
             base.t == T_NULL ? "null" : base.t == T_INT ? "int" :
             base.t == T_FLOAT ? "float" :
             (base.t == T_TRUE || base.t == T_FALSE) ? "bool" : "value");
}

static int32_t op_fetch_dim_is(VM *vm, TOp *op) {
    TVal base = rd(vm, op, 1);            /* silent: T_UNDEF ok */
    TVal out = TV_NULL;
    if (op->t2 != OPD_UNUSED) {
        TVal key = rd(vm, op, 2);
        if (base.t == T_ARR) {
            TArr *a = (TArr *)base.u.p;
            if (tval_norm_key(&key)) {
                TVal *v = tarr_get(a, key);
                if (v) { out = *v; tval_addref(out); }
            }
        } else if (base.t == T_STR) {
            TStr *s = (TStr *)base.u.p;
            if (tval_norm_key(&key) && key.t == T_INT) {
                int64_t i = key.u.i;
                if (i < 0) i += s->len;
                if (i >= 0 && i < (int64_t)s->len)
                    out = TV_STR(tstr_new(s->c + i, 1));
            }
        }
    }
    wtake(vm, op, out);
    return 1;
}

static int32_t op_isset_dim(VM *vm, TOp *op) {
    TVal base = rd(vm, op, 1);
    bool has = false;
    if (op->t2 != OPD_UNUSED) {
        TVal key = rd(vm, op, 2);
        if (base.t == T_ARR) {
            if (tval_norm_key(&key))
                has = tarr_has((TArr *)base.u.p, key);
        } else if (base.t == T_STR) {
            TStr *s = (TStr *)base.u.p;
            if (tval_norm_key(&key) && key.t == T_INT) {
                int64_t i = key.u.i;
                if (i < 0) i += s->len;
                has = i >= 0 && i < (int64_t)s->len;
            }
        }
    }
    wtake(vm, op, has ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_fetch_global(VM *vm, TOp *op) {
    TVal *g = global_slot(vm, (TStr *)vm->x.frame->fn->lits[op->a2].u.p);
    if (!g || g->t == T_UNDEF)
        vm_raise(vm, ex_class("UndefinedError"), op, "Undefined variable $%s",
                 tstr_val((TStr *)vm->x.frame->fn->lits[op->a2].u.p));
    TVal out = *g;
    tval_addref(out);
    wtake(vm, op, out);
    return 1;
}

static int32_t op_assign_global(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    TVal *g = global_slot(vm, (TStr *)vm->x.frame->fn->lits[op->a2].u.p);
    if (!g)
        vm_raise(vm, ex_class("Error"), op, "Internal: global slot missing");
    tval_addref(v);
    tval_release(g);
    *g = v;
    return 1;
}

static int32_t op_isset_global(VM *vm, TOp *op) {
    TVal *g = global_slot(vm, (TStr *)vm->x.frame->fn->lits[op->a2].u.p);
    bool has = g && g->t != T_UNDEF;
    wtake(vm, op, has ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_unset_global(VM *vm, TOp *op) {
    TVal *g = global_slot(vm, (TStr *)vm->x.frame->fn->lits[op->a2].u.p);
    if (g) {
        tval_release(g);
        *g = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    return 1;
}

static int32_t op_isset_cv(VM *vm, TOp *op) {
    TVal *p = &vm->x.frame->slots[op->a1];
    wtake(vm, op, p->t != T_UNDEF ? TV_TRUE : TV_FALSE);
    return 1;
}

static int32_t op_unset_cv(VM *vm, TOp *op) {
    TVal *p = &vm->x.frame->slots[op->a1];
    tval_release(p);
    *p = (TVal){ .u.i = 0, .t = T_UNDEF };
    return 1;
}

/* ---- assignment ---- */
static int32_t op_assign_dim(VM *vm, TOp *op) {
    return do_dim_write(vm, op);
}

static int32_t op_unset_dim(VM *vm, TOp *op) {
    TVal *bp = opd(vm, op, 1);
    if (bp->t != T_ARR) return 1;         /* unset on non-array: no-op */
    TArr *a = dim_cow(vm, bp);
    TVal key = rd(vm, op, 2);
    if (tval_norm_key(&key))
        tarr_unset(a, key);
    return 1;
}

/* ---- increment / decrement ---- */
static int32_t do_incdec(VM *vm, TOp *op) {
    bool is_inc = op->op == OP_INC;
    if (op->t2 == OPD_UNUSED) {
        /* variable read-modify-write: o1 slot is updated in place */
        TVal *p = opd(vm, op, 1);
        TVal old = p->t == T_UNDEF ? TV_NULL : *p;
        TVal nv = is_inc ? tval_inc(old) : tval_dec(old);
        tval_addref(nv);
        TVal res = op->ext ? nv : old;    /* prefix -> new value */
        tval_addref(res);
        tval_release(p);
        *p = nv;
        if (op->tr != OPD_UNUSED) {
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            *d = res;
        } else {
            tval_release(&res);
        }
        return 1;
    }
    /* dim: base slot is COW-separated in place */
    TVal *bp = opd(vm, op, 1);
    if (bp->t == T_UNDEF)
        *bp = TV_ARR(tarr_new());
    if (bp->t != T_ARR)
        vm_raise(vm, ex_class("TypeError"), op,
                 "Cannot increment/decrement a %s offset",
                 bp->t == T_STR ? "string" : "scalar");
    TArr *a = dim_cow(vm, bp);
    TVal key = rd(vm, op, 2);
    if (!tval_norm_key(&key))
        vm_raise(vm, ex_class("TypeError"), op, "Illegal offset type");
    TVal *cur = tarr_get(a, key);
    TVal old = cur ? *cur : TV_NULL;
    TVal nv = is_inc ? tval_inc(old) : tval_dec(old);
    tval_addref(nv);
    tarr_set(a, key, nv);
    TVal res = op->ext ? nv : old;
    tval_addref(res);
    wtake(vm, op, res);
    return 1;
}

static int32_t op_inc(VM *vm, TOp *op) { return do_incdec(vm, op); }
static int32_t op_dec(VM *vm, TOp *op) { return do_incdec(vm, op); }

/* ---- arrays ---- */
static int32_t op_array_init(VM *vm, TOp *op) {
    wtake(vm, op, TV_ARR(tarr_new()));
    return 1;
}

static int32_t op_array_append(VM *vm, TOp *op) {
    return do_dim_write(vm, op);
}

static int32_t op_array_spread(VM *vm, TOp *op) {
    TVal src = rdv(vm, op, 2);
    if (src.t != T_ARR)
        vm_raise(vm, ex_class("TypeError"), op,
                 "Only arrays can be spread in array literals");
    TVal *dp = opd(vm, op, 1);
    TArr *a = dim_cow(vm, dp);
    TArr *sa = (TArr *)src.u.p;
    for (uint32_t i = 0; i < sa->dused; i++) {
        if (sa->data[i].t == T_UNDEF) continue;
        TVal k = arr_key_at(sa, i);
        TVal v = sa->data[i];
        tval_addref(v);
        /* PHP: integer keys are re-indexed on spread, string keys kept */
        if (k.t == T_INT)
            tarr_append(a, v);
        else
            tarr_set(a, k, v);
    }
    return 1;
}

/* ---- iteration ---- */
static int32_t op_fe_reset(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    if (v.t != T_ARR)
        vm_raise(vm, ex_class("TypeError"), op,
                 "foreach iterates over value of type %s",
                 v.t == T_NULL ? "null" : v.t == T_STR ? "string" :
                 v.t == T_INT ? "int" : v.t == T_FLOAT ? "float" : "bool");
    tval_addref(v);
    wtake(vm, op, v);
    return 1;
}

static int32_t op_fe_fetch(VM *vm, TOp *op) {
    TVal it = rd(vm, op, 1);
    TArr *a = (TArr *)it.u.p;
    TVal *cur = opd(vm, op, 2);
    int64_t i = cur->u.i;
    while (i < a->dused && a->data[i].t == T_UNDEF) i++;
    if (i >= a->dused) {
        cur->u.i = i;
        wtake(vm, op, TV_FALSE);
        return 1;
    }
    TVal val = a->data[i];
    tval_addref(val);
    TVal *vp = &vm->x.frame->slots[op->a2 + 1];
    tval_release(vp);
    *vp = val;
    TVal key = arr_key_at(a, (uint32_t)i);
    tval_addref(key);
    TVal *kp = &vm->x.frame->slots[op->a2 + 2];
    tval_release(kp);
    *kp = key;
    cur->u.i = i + 1;
    wtake(vm, op, TV_TRUE);
    return 1;
}

/* shared: called_cls resolution for a frame being created */
static TClass *frame_called_cls(VMCallRec *rec, TFunc *fn);

static bool vm_call_magic(VM *vm, TOp *op, TObj *o, TClass *owner,
                          const char *name, TVal *args, int n, TVal *out);

/* ---- calls ---- */
static VMCallRec *call_push(VM *vm, TOp *op) {
    if (vm->x.n_calls >= VM_MAX_CALLS)
        vm_raise(vm, ex_class("Error"), op, "Call stack overflow");
    VMCallRec *rec = &vm->x.calls[vm->x.n_calls++];
    memset(rec, 0, sizeof *rec);
    rec->args_base = (uint32_t)vm->x.n_args;
    rec->n_args = (uint16_t)op->a2;
    rec->line = op->line;
    return rec;
}

/* resolve a function name to a user TFunc or a builtin; NULL = not found.
 * ns_fallback: unqualified-in-namespace calls (ext=1) retry the bare name,
 * per PHP's `A\f()` -> `f()` fallback rule. */
/* 缺省值落入变量槽：interned/int 直接 addref（TFunc 共享，跨线程只读
 * 安全，M5）；堆数组深拷（共享 defaults 不可被任何线程 mutate / 写 rc）。
 * 数组默认值罕见，深拷成本可忽略 */
static TVal default_arg_val(TVal d) {
    if (d.t == T_ARR) return tval_clone_deep(d);
    tval_addref(d);
    return d;
}

static void *resolve_fn(VM *vm, TStr *name, bool *is_builtin) {
    for (int i = 0; i < vm->prog->n_funcs; i++)
        if (vm->prog->funcs[i]->name &&
            !vm->prog->funcs[i]->is_closure &&
            tstr_eq(vm->prog->funcs[i]->name, name)) {
            *is_builtin = false;
            return vm->prog->funcs[i];
        }
    for (int i = 0; i < tphp_builtin_count; i++)
        if (strcmp(tphp_builtins[i].name, tstr_val(name)) == 0) {
            *is_builtin = true;
            return (void *)tphp_builtins[i].fn;
        }
    return NULL;
}

/* last path segment after the final '\' (bare global name) */
static TStr *ns_bare_name(TStr *name) {
    const char *s = tstr_val(name);
    const char *last = strrchr(s, '\\');
    if (!last) return name;
    return tstr_intern(last + 1, strlen(last + 1));
}

static void *resolve_fn_fb(VM *vm, TOp *op, TStr *name, bool *is_builtin) {
    void *hit = resolve_fn(vm, name, is_builtin);
    if (hit) return hit;
    if (op->ext == 1) return resolve_fn(vm, ns_bare_name(name), is_builtin);
    return NULL;
}

static int32_t op_init_fcall(VM *vm, TOp *op) {
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a1].u.p;
    VMCallRec *rec = call_push(vm, op);
    /* inline cache: user TFunc* (aligned) or builtin fn | 1 */
    if (op->ic) {
        if ((uintptr_t)op->ic & 1) {
            rec->is_builtin = true;
            rec->bf = (BuiltinFn)((uintptr_t)op->ic & ~(uintptr_t)1);
        } else {
            rec->uf = (TFunc *)op->ic;
        }
        rec->dst_kind = op->tr;
        rec->dst_slot = op->ar;
        return 1;
    }
    bool is_builtin;
    void *hit = resolve_fn_fb(vm, op, name, &is_builtin);
    if (!hit) {
        vm->x.n_calls--;
        vm_raise(vm, ex_class("Error"), op,
                 "Call to undefined function %s()",
                 tstr_val(name));
    }
    if (is_builtin) {
        rec->is_builtin = true;
        rec->bf = (BuiltinFn)hit;
        op->ic = (void *)((uintptr_t)hit | 1);
    } else {
        rec->uf = (TFunc *)hit;
        op->ic = hit;
    }
    rec->dst_kind = op->tr;
    rec->dst_slot = op->ar;
    return 1;
}

static int32_t op_init_dyn(VM *vm, TOp *op) {
    TVal fv = rdv(vm, op, 1);
    VMCallRec *rec = call_push(vm, op);
    rec->dst_kind = op->tr;
    rec->dst_slot = op->ar;
    if (fv.t == T_CLOSURE) {
        rec->clo = (TClosure *)fv.u.p;
        return 1;
    }
    if (fv.t == T_OBJ) {
        /* __invoke: an object used as `$obj(...)` */
        GCHead *h = (GCHead *)fv.u.p;
        if (h->kind == GC_KIND_OBJ) {
            TObj *o = (TObj *)h;
            TMethod *m = tphp_class_find_method(o->cls, "__invoke");
            if (m && m->fn && !m->is_abstract) {
                rec->is_method = true;
                rec->this_obj = o;
                tval_addref(fv);          /* frame slot 0 owns a reference */
                rec->uf = m->fn;
                return 1;
            }
        }
        vm_raise(vm, ex_class("Error"), op, "Value is not callable");
    }
    if (fv.t != T_STR)
        vm_raise(vm, ex_class("TypeError"), op, "Function name must be a string");
    TStr *name = (TStr *)fv.u.p;
    bool is_builtin;
    void *hit = resolve_fn(vm, name, &is_builtin);
    if (!hit) {
        vm->x.n_calls--;
        vm_raise(vm, ex_class("Error"), op,
                 "Call to undefined function %s()",
                 tstr_val(name));
    }
    if (is_builtin) {
        rec->is_builtin = true;
        rec->bf = (BuiltinFn)hit;
    } else {
        rec->uf = (TFunc *)hit;
    }
    return 1;
}

static void args_push(VM *vm, TOp *op, TVal v) {
    if (vm->x.n_args >= VM_MAX_ARGS)
        vm_raise(vm, ex_class("Error"), op, "Too many arguments");
    tval_addref(v);
    vm->x.args[vm->x.n_args++] = v;
}

static int32_t op_send_val(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    args_push(vm, op, v);
    return 1;
}

static int32_t op_send_cap(VM *vm, TOp *op) {
    TVal v = rd(vm, op, 1);       /* raw: capturing an undefined var -> null */
    if (v.t == T_UNDEF) v = TV_NULL;
    if (vm->x.n_caps >= 256)
        vm_raise(vm, ex_class("Error"), op, "Too many closure captures");
    tval_addref(v);
    vm->x.caps[vm->x.n_caps++] = v;
    return 1;
}

static int32_t op_send_spread(VM *vm, TOp *op) {
    TVal src = rdv(vm, op, 1);
    if (src.t != T_ARR)
        vm_raise(vm, ex_class("TypeError"), op,
                 "Only arrays can be spread in argument lists");
    TArr *a = (TArr *)src.u.p;
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        args_push(vm, op, a->data[i]);
    }
    return 1;
}

static int32_t op_recv(VM *vm, TOp *op) {
    VMFrame *f = vm->x.frame;
    uint32_t idx = op->a1;
    TVal v;
    if (idx < f->n_args_sent) {
        v = vm->x.args[f->args_base + idx];
        if (v.t == T_UNDEF) {
            /* named-arg hole: fall back to default / thunk / arity error */
            TVal d = f->fn->defaults[idx];
            if (d.t == T_UNDEF && f->fn->default_thunk && f->fn->default_thunk[idx]) {
                v = tphp_call_thunk(vm, f->fn->default_thunk[idx]);
            } else if (d.t == T_UNDEF) {
                vm_raise(vm, ex_class("ArgumentCountError"), op,
                         "Too few arguments to function %s()",
                         tstr_val(f->fn->name));
            } else {
                v = default_arg_val(d);
            }
        } else {
            /* move */
            vm->x.args[f->args_base + idx] = (TVal){ .u.i = 0, .t = T_UNDEF };
        }
    } else {
        TVal d = f->fn->defaults[idx];
        if (d.t == T_UNDEF && f->fn->default_thunk && f->fn->default_thunk[idx]) {
            v = tphp_call_thunk(vm, f->fn->default_thunk[idx]);
        } else if (d.t == T_UNDEF) {
            vm_raise(vm, ex_class("ArgumentCountError"), op,
                     "Too few arguments to function %s()",
                     tstr_val(f->fn->name));
        } else {
            v = default_arg_val(d);
        }
    }
    TVal *d = &vm->x.frame->slots[op->ar];
    tval_release(d);
    *d = v;
    return 1;
}

static int32_t op_recv_variadic(VM *vm, TOp *op) {
    VMFrame *f = vm->x.frame;
    uint32_t fixed = op->a1;
    TArr *a = tarr_new();
    uint32_t to = f->n_args_sent > fixed ? f->n_args_sent : fixed;
    for (uint32_t i = fixed; i < to; i++) {
        TVal v = vm->x.args[f->args_base + i];           /* move */
        vm->x.args[f->args_base + i] = (TVal){ .u.i = 0, .t = T_UNDEF };
        if (v.t != T_UNDEF) tarr_append(a, v);
        else tval_release(&v);
    }
    wtake(vm, op, TV_ARR(a));
    return 1;
}

/* forward decls used before definition */
static bool lc_eq(const char *a, const char *b);

static int32_t op_do_fcall(VM *vm, TOp *op) {
    VMCallRec *rec = &vm->x.calls[vm->x.n_calls - 1];
    rec->n_args = (uint16_t)(vm->x.n_args - rec->args_base);  /* real count */
    TVal *args = &vm->x.args[rec->args_base];
    if (rec->is_enum_builtin) {
        /* enum ::from / ::tryFrom / ::cases */
        TClass *cls = (TClass *)rec->called_cls;
        /* recover the method name from the caller's INIT op */
        TOp *init_op = op - 1 - rec->n_args;
        TStr *key = (TStr *)vm->x.frame->fn->lits[init_op->ar].u.p;
        const char *s = tstr_val(key);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2)
            sep = p;
        const char *mn = sep ? sep + 2 : s;
        TVal r = TV_NULL;
        if (lc_eq(mn, "cases")) {
            TArr *arr = tarr_new();
            for (int i = 0; i < cls->n_cases; i++)
                tarr_append(arr, TV_OBJ(tphp_enum_case_obj(cls, i)));
            r = TV_ARR(arr);
        } else {
            TVal probe = rec->n_args > 0 ? args[0] : TV_NULL;
            TEnumCase *hit = NULL;
            for (int i = 0; i < cls->n_cases; i++)
                if (cls->cases[i].backing.t != T_NULL &&
                    tval_loose_eq(cls->cases[i].backing, probe)) {
                    hit = &cls->cases[i];
                    break;
                }
            if (hit) r = TV_OBJ(tphp_enum_case_obj(cls, (int)(hit - cls->cases)));
            else if (lc_eq(mn, "from"))
                vm_raise(vm, ex_class("Error"), op,
                         "\"%s\" is not a valid backing value for enum %s",
                         tstr_val(tval_to_str(probe)), tstr_val(cls->name));
        }
        for (int i = 0; i < rec->n_args; i++)
            if (args[i].t != T_UNDEF) tval_release(&args[i]);
        vm->x.n_args = (int)rec->args_base;
        vm->x.n_calls--;
        if (op->tr != OPD_UNUSED) {
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            d[0] = r;
        } else {
            tval_release(&r);
        }
        return 1;
    }
    if (rec->is_ex_getter) {
        /* INIT_MCALL precomputed an exception-getter value */
        TVal r = rec->getter_val;
        vm->x.n_args = (int)rec->args_base;
        vm->x.n_calls--;
        if (op->tr != OPD_UNUSED) {
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            *d = r;
        } else {
            tval_release(&r);
        }
        return 1;
    }
    if (rec->magic_kind) {
        /* __call / __callStatic: the INIT stamped the member name; pack
         * (name, args-array) and run the magic method */
        TStr *mname = rec->magic_name;
        TObj *o = (TObj *)rec->this_obj;
        rec->this_obj = NULL;
        TClass *owner = rec->magic_kind == 1 ? o->cls : (TClass *)rec->called_cls;
        TVal pack[2];
        pack[0] = TV_STR(mname);
        tval_addref(pack[0]);
        TArr *arr = tarr_new();
        for (int i = 0; i < rec->n_args; i++) {
            TVal v = args[i];
            tval_addref(v);               /* the array takes its own ref */
            tarr_append(arr, v);
            if (v.t != T_UNDEF) tval_release(&args[i]);
        }
        pack[1] = TV_ARR(arr);
        vm->x.n_args = (int)rec->args_base;
        vm->x.n_calls--;
        TVal out;
        bool ok = vm_call_magic(vm, op, rec->magic_kind == 1 ? o : NULL,
                                owner, rec->magic_kind == 1 ? "__call" : "__callstatic",
                                pack, 2, &out);
        if (rec->magic_kind == 1 && o) {
            TVal ov = TV_OBJ(o);
            tval_release(&ov);           /* drop the call record's reference */
        }
        /* pack ownership was MOVED into the callee (RECV takes the args-area
         * refs, RETURN releases them) — releasing here would double-free */
        if (!ok)
            vm_raise(vm, ex_class("Error"), op, "Magic method vanished");
        if (op->tr != OPD_UNUSED) {
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            *d = out;
        } else {
            tval_release(&out);
        }
        return 1;
    }
    BuiltinFn clo_bfn = (rec->clo && rec->clo->bfn) ? (BuiltinFn)rec->clo->bfn : NULL;
    if (rec->is_builtin || clo_bfn) {
        TVal r;
        if (rec->ffi_fn && !clo_bfn) {
            /* M5.5 FFI 符号调用：阻塞 native，不挂起 */
            void *ffn = rec->ffi_fn;
            TObj *ro = (TObj *)rec->this_obj;
            rec->this_obj = NULL;
            rec->ffi_fn = NULL;
            r = tphp_ffi_call_fn(vm, ffn, args, rec->n_args);
            if (ro) {
                TVal rv = TV_OBJ(ro);
                tval_release(&rv);
            }
        } else if (rec->nm_fn && !clo_bfn) {
            /* internal-class native method: receiver + args (NULL receiver
             * = static native, M5-c Worker::self) */
            NativeMethodFn nmf = (NativeMethodFn)rec->nm_fn;
            TObj *ro = (TObj *)rec->this_obj;
            rec->this_obj = NULL;
            r = nmf(vm, ro, args, rec->n_args);
            if (ro) {
                TVal rv = TV_OBJ(ro);
                tval_release(&rv);    /* drop the call record's receiver ref */
            }
        } else {
            BuiltinFn bf = clo_bfn ? clo_bfn : rec->bf;
            r = bf(vm, args, rec->n_args);
        }
        for (int i = 0; i < rec->n_args; i++)
            if (args[i].t != T_UNDEF) tval_release(&args[i]);
        vm->x.n_args = (int)rec->args_base;
        vm->x.n_calls--;
        if (op->tr != OPD_UNUSED) {
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            *d = r;
        } else {
            tval_release(&r);
        }
        return 1;
    }
    /* user function or closure; the return destination rides on THIS op */
    rec->dst_kind = op->tr;
    rec->dst_slot = op->ar;
    TFunc *fn = rec->clo ? rec->clo->fn : rec->uf;
    /* variadic：超出固定形参的实参由 RECV_VARIADIC 搬走，不能在这里释放
     * （先释放后搬运 = 悬垂指针，refcounted 值为潜伏 UAF） */
    if (!fn->is_variadic)
        for (uint16_t i = fn->n_params; i < rec->n_args; i++)
            if (args[i].t != T_UNDEF) tval_release(&args[i]);
    vm->x.n_args = (int)rec->args_base;
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = fn;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = rec->args_base;
    f->n_args_sent = rec->n_args;
    f->n_calls_at_entry = vm->x.n_calls;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    if (rec->clo)
        for (uint16_t i = 0; i < rec->clo->n_cap; i++) {
            tval_addref(rec->clo->captures[i]);
            f->slots[i] = rec->clo->captures[i];
        }
    vm->x.frame->pc++;              /* caller resumes after DO_FCALL */
    vm->x.frame = f;
    f->called_cls = frame_called_cls(rec, fn);
    if (rec->is_method && rec->this_obj) {
        /* $this lives in slot 0 (after capture slots, which methods lack);
         * ownership of the reference moves from the call record */
        f->slots[0] = TV_OBJ((TObj *)rec->this_obj);
        rec->this_obj = NULL;
    }
    return 0;                     /* pc managed (new frame starts at 0) */
}

static int32_t op_return(VM *vm, TOp *op) {
    TVal r = TV_NULL;
    if (op->t1 != OPD_UNUSED) {
        r = rdv(vm, op, 1);
        tval_addref(r);
    }
    if (vm->x.n_calls > 0) {
        VMCallRec *rec = &vm->x.calls[vm->x.n_calls - 1];
        vm->x.n_calls--;
        if (rec->is_ctor) {
            /* `new` always yields the object itself, never the ctor's
             * return value (PHP semantics) */
            tval_release(&r);
            TVal objv = vm->x.frame->slots[0];   /* $this */
            tval_addref(objv);
            TVal *d = &vm->x.frame->prev->slots[rec->dst_slot];
            tval_release(d);
            *d = objv;
        } else if (rec->is_thunk) {
            vm->x.thunk_ret = r;    /* ownership moves to the thunk caller */
        } else if (rec->dst_kind != OPD_UNUSED) {
            TVal *d = &vm->x.frame->prev->slots[rec->dst_slot];
            tval_release(d);
            *d = r;
        } else {
            tval_release(&r);
        }
    } else if (vm->x.coro_result) {
        /* coroutine root frame (M4): the bottom RETURN yields the value */
        *vm->x.coro_result = r;   /* ownership moves to the coroutine */
    } else {
        tval_release(&r);         /* main-frame return value discarded */
    }
    VMFrame *f = vm->x.frame;
    struct VMFrame *prev = f->prev;
    TFunc *fn = f->fn;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    for (uint32_t i = 0; i < nslots; i++)
        tval_release(&f->slots[i]);
    tm_free(f->slots);
    tm_free(f);
    vm->x.frame = prev;
    if (!vm->x.frame) vm->x.done = true;
    return 0;
}

/* ---- constants ---- */
static int32_t op_define_const(VM *vm, TOp *op) {
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a1].u.p;
    TVal v = rdv(vm, op, 2);
    for (int i = 0; i < vm->n_consts; i++)
        if (tstr_eq(vm->consts[i].name, name))
            vm_raise(vm, ex_class("Error"), op,
                     "Constant %s already defined", tstr_val(name));
    if (vm->n_consts >= 256)
        vm_raise(vm, ex_class("Error"), op, "Too many constants");
    tval_addref(v);
    vm->consts[vm->n_consts].name = name;
    vm->consts[vm->n_consts].v = v;
    vm->n_consts++;
    return 1;
}

static int32_t op_fetch_const(VM *vm, TOp *op) {
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a1].u.p;
    for (int i = 0; i < vm->n_consts; i++) {
        if (tstr_eq(vm->consts[i].name, name)) {
            TVal v = vm->consts[i].v;
            tval_addref(v);
            wtake(vm, op, v);
            return 1;
        }
    }
    /* namespace fallback: unqualified-in-NS constants retry the bare name */
    if (op->ext == 1) {
        TStr *bare = ns_bare_name(name);
        for (int i = 0; i < vm->n_consts; i++) {
            if (tstr_eq(vm->consts[i].name, bare)) {
                TVal v = vm->consts[i].v;
                tval_addref(v);
                wtake(vm, op, v);
                return 1;
            }
        }
    }
    vm_raise(vm, ex_class("Error"), op,
             "Undefined constant \"%s\"", tstr_val(name));
}

/* ---- M2: closures, exceptions, first-class ---- */
static int32_t op_closure_init(VM *vm, TOp *op) {
    uint32_t fn_idx = op->a1;
    uint16_t n_cap = (uint16_t)op->a2;
    if (fn_idx >= (uint32_t)vm->prog->n_funcs)
        vm_raise(vm, ex_class("Error"), op, "Internal: closure function missing");
    TFunc *fn = vm->prog->funcs[fn_idx];
    if (vm->x.n_caps < n_cap)
        vm_raise(vm, ex_class("Error"), op, "Internal: capture stack underflow");
    TClosure *c = tclo_alloc(fn, &vm->x.caps[vm->x.n_caps - n_cap], n_cap);
    for (int i = 0; i < n_cap; i++)
        tval_release(&vm->x.caps[vm->x.n_caps - n_cap + i]);
    vm->x.n_caps -= n_cap;
    wtake(vm, op, TV_CLO(c));
    return 1;
}

static int32_t op_get_func(VM *vm, TOp *op) {
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a1].u.p;
    bool is_builtin;
    void *hit = resolve_fn_fb(vm, op, name, &is_builtin);
    if (!hit)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot create first-class callable for %s()", tstr_val(name));
    if (is_builtin) {
        TClosure *c = tclo_alloc_builtin((BuiltinFn)hit);
        wtake(vm, op, TV_CLO(c));
        return 1;
    }
    TClosure *c = tclo_alloc((TFunc *)hit, NULL, 0);
    wtake(vm, op, TV_CLO(c));
    return 1;
}

/* first-class callable from a dynamic value ($f(...)): closures pass
 * through unchanged; string function names convert to a closure */
static int32_t op_fcc_dyn(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    if (v.t == T_CLOSURE) {
        tval_addref(v);
        wtake(vm, op, v);
        return 1;
    }
    if (v.t != T_STR)
        vm_raise(vm, ex_class("Error"), op,
                 "Value is not a callable (first-class syntax)");
    TStr *name = (TStr *)v.u.p;
    bool is_builtin;
    void *hit = resolve_fn(vm, name, &is_builtin);
    if (!hit)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot create first-class callable for %s()", tstr_val(name));
    TClosure *c = is_builtin ? tclo_alloc_builtin((BuiltinFn)hit)
                             : tclo_alloc((TFunc *)hit, NULL, 0);
    tval_release(&v);
    wtake(vm, op, TV_CLO(c));
    return 1;
}

static int32_t op_new_ex(VM *vm, TOp *op) {
    uint32_t ci = op->a1;
    if (ci >= (uint32_t)tphp_ex_class_count)
        vm_raise(vm, ex_class("Error"), op, "Internal: exception class missing");
    const TExClass *cls = tphp_ex_classes[ci];
    TVal *args = vm->x.args + vm->x.n_args - (int)op->a2;
    TStr *msg = tstr_new("", 0);
    int64_t code = 0;
    if (op->a2 > 0) {
        if (args[0].t != T_STR)
            vm_raise(vm, ex_class("TypeError"), op,
                     "Exception message must be a string");
        msg = (TStr *)args[0].u.p;
        tval_addref(TV_STR(msg));
    }
    if (op->a2 > 1 && args[1].t == T_INT) code = args[1].u.i;
    TExObj *prev = NULL;
    if (op->a2 > 2 && args[2].t == T_OBJ) {
        prev = (TExObj *)args[2].u.p;
        prev->h.rc++;                       /* exception owns the chain */
    }
    /* consume the queued ctor args (msg/prev took their own references):
     * release the area slots and pop them off, or the next call would
     * inherit them as phantom arguments */
    {
        uint32_t base = vm->x.n_args - (int)op->a2;
        for (uint32_t i = 0; i < op->a2; i++)
            if (vm->x.args[base + i].t != T_UNDEF)
                tval_release(&vm->x.args[base + i]);
        vm->x.n_args = (int)base;
    }
    wtake(vm, op, TV_OBJ(tex_alloc(cls, msg, code, op->line, prev)));
    return 1;
}

static int32_t op_throw(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    if (v.t != T_OBJ || ((GCHead *)v.u.p)->kind != GC_KIND_EX)
        vm_raise(vm, ex_class("TypeError"), op,
                 "Can only throw objects");
    TExObj *x = (TExObj *)v.u.p;
    x->line = op->line;
    tval_addref(v);
    vm->x.exception = x;
    longjmp(vm->x.unwind, 1);
}

static int32_t op_bind_ex(VM *vm, TOp *op) {
    TExObj *x = vm->x.exception;
    vm->x.exception = NULL;         /* ownership moves into the catch slot */
    TVal *d = &vm->x.frame->slots[op->ar];
    tval_release(d);
    d[0] = TV_OBJ(x);
    return 1;
}

/* ---- M3: objects ---- */
static bool lc_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(a[0] + 32) : a[0];
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(b[0] + 32) : b[0];
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static const char *vis_name(uint8_t vis) {
    return vis == CVIS_PRIVATE ? "private" :
           vis == CVIS_PROTECTED ? "protected" : "public";
}

/* visibility scope class for the running frame (NULL = global scope) */
static TClass *frame_scope_cls(VM *vm) {
    return vm->x.frame ? (TClass *)vm->x.frame->called_cls : NULL;
}

/* exception getters callable as methods (M2 sugar): getMessage() etc */
static bool ex_getter(const char *n, TExObj *x, TVal *out) {
    if (lc_eq(n, "getMessage"))   { *out = TV_STR(x->message); tval_addref(*out); return true; }
    if (lc_eq(n, "getCode"))      { *out = TV_INT(x->code); return true; }
    if (lc_eq(n, "getPrevious"))  { *out = x->previous ? TV_OBJ(x->previous) : TV_NULL;
                                    if (x->previous) tval_addref(*out); return true; }
    if (lc_eq(n, "getLine"))      { *out = TV_INT(x->line); return true; }
    return false;
}

/* ---- magic-method invocation helper (C-level method call) ----
 * Runs method `name` (looked up on `owner`) with `args` (n of them, MOVED:
 * the callee consumes the references — RECV takes them into the parameter
 * slots, RETURN releases them; on a raise the queued args are released
 * here). `o` is the receiver, NULL for static (__callStatic). The caller
 * must NOT release the args again. Returns false when the method does not
 * exist. */
static bool vm_call_magic(VM *vm, TOp *op, TObj *o, TClass *owner,
                          const char *name, TVal *args, int n, TVal *out) {
    TMethod *m = tphp_class_find_method(owner, name);
    if (!m || !m->fn || m->is_abstract) return false;
    if (vm->x.n_args + n > VM_MAX_ARGS)
        vm_raise(vm, ex_class("Error"), op, "Too many arguments");
    uint32_t base = (uint32_t)vm->x.n_args;
    for (int i = 0; i < n; i++)
        vm->x.args[base + (uint32_t)i] = args[i];
    vm->x.n_args = (int)base + n;
    VMCallRec *rec = call_push(vm, op);
    rec->is_method = true;
    rec->is_thunk = true;         /* RETURN stores into vm->x.thunk_ret */
    rec->uf = m->fn;
    TFunc *fn = m->fn;
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = fn;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = base;
    f->n_args_sent = (uint16_t)n;
    f->called_cls = owner;
    f->n_calls_at_entry = vm->x.n_calls;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    if (o) {
        f->slots[0] = TV_OBJ(o);
        tval_addref(f->slots[0]);
    }
    /* caller pc advancement is the invoking handler's job (returns 1) */
    vm->x.frame = f;
    /* run to completion of this frame (handles nested throws via unwind) */
    while (vm->x.frame == f) {
        TOp *cop = &f->fn->ops[f->pc];
        int32_t d = ((OpHandler)cop->handler)(vm, cop);
        if (d) f->pc += (uint32_t)d;
    }
    vm->x.n_args = (int)base;
    if (vm->x.exception) {
        /* __get etc raised: drop the queued args and propagate through
         * the normal unwind path */
        for (int i = 0; i < n; i++)
            tval_release(&vm->x.args[base + (uint32_t)i]);
        longjmp(vm->x.unwind, 1);
    }
    *out = vm->x.thunk_ret;
    return true;
}

static int32_t op_init_mcall(VM *vm, TOp *op) {
    TVal objv = rdv(vm, op, 1);
    TStr *mname = (TStr *)vm->x.frame->fn->lits[op->ar].u.p;
    VMCallRec *rec = call_push(vm, op);
    if (objv.t != T_OBJ || objv.u.p == NULL)
        vm_raise(vm, ex_class("Error"), op,
                 "Call to a member function %s() on %s",
                 tstr_val(mname),
                 objv.t == T_NULL ? "null" :
                 objv.t == T_STR ? "string" :
                 objv.t == T_INT ? "int" :
                 objv.t == T_FLOAT ? "float" :
                 objv.t == T_ARR ? "array" : "bool");
    GCHead *h = (GCHead *)objv.u.p;
    if (h->kind == GC_KIND_EX) {
        /* M2 exception getters keep working through the method-call path */
        TVal out;
        if (ex_getter(tstr_val(mname), (TExObj *)h, &out)) {
            rec->is_ex_getter = true;
            rec->getter_val = out;
            return 1;
        }
        vm_raise(vm, ex_class("Error"), op,
                 "Undefined method %s() on exceptions in M3",
                 tstr_val(mname));
    }
    if (h->kind != GC_KIND_OBJ)
        vm_raise(vm, ex_class("Error"), op,
                 "Method calls on this object type land in M3");
    TObj *o = (TObj *)h;
    TMethod *m = tphp_class_find_method(o->cls, tstr_val(mname));
    /* owner is the lookup class (flat table carries no declaring-class);
     * M3 difference: a parent-declared private method stays callable from
     * subclasses because owner == from collapses the check */
    bool m_hidden = m && !tphp_vis_ok(m->visibility, o->cls,
                                      frame_scope_cls(vm));
    if (m && !m->fn && !m->is_abstract && m->native) {
        /* internal-class native method (M4): dispatched at DO_FCALL with
         * the full argument area; the receiver rides on this_obj */
        rec->is_builtin = true;
        rec->nm_fn = m->native;
        rec->is_method = true;
        rec->this_obj = o;
        tval_addref(objv);            /* frame slot 0 owns a reference */
        return 1;
    }
    /* M5.5: FFI 对象上未声明的方法 → 按 cdef 符号分发 */
    if (tphp_ffi_try_mcall(vm, o, mname, rec))
        return 1;
    if (!m || !m->fn || m->is_abstract || m_hidden) {
        /* __call(name, args) fallback: defer to DO_FCALL where every
         * argument has been SENDed (packing here would see an empty area) */
        TMethod *mc = tphp_class_find_method(o->cls, "__call");
        if (mc && mc->fn && !mc->is_abstract) {
            rec->magic_kind = 1;
            rec->magic_name = mname;
            rec->is_method = true;
            rec->this_obj = o;
            tval_addref(objv);            /* frame slot 0 owns a reference */
            return 1;
        }
        if (m_hidden)
            vm_raise(vm, ex_class("Error"), op,
                     "Call to %s method %s::%s() from scope %s",
                     vis_name(m->visibility), tstr_val(o->cls->name),
                     tstr_val(mname),
                     frame_scope_cls(vm) ? tstr_val(frame_scope_cls(vm)->name) : "global");
        vm_raise(vm, ex_class("Error"), op,
                 "Call to undefined method %s::%s()",
                 tstr_val(o->cls->name), tstr_val(mname));
    }
    rec->is_method = true;
    rec->this_obj = o;
    tval_addref(objv);            /* frame slot 0 owns a reference */
    rec->uf = m->fn;
    return 1;
}

static int32_t op_new_user(VM *vm, TOp *op) {
    TStr *cname = (TStr *)vm->x.frame->fn->lits[op->a1].u.p;
    char lc[128];
    { const char *s = tstr_val(cname);
      size_t k = 0; for (; s[k] && k < sizeof lc - 1; k++)
          lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
      lc[k] = 0; }
    /* class_resolve_lc runs spl_autoload hooks; declared in class.c */
    extern TClass *class_resolve_lc(const char *fqcn_lc);
    TClass *cls = class_resolve_lc(lc);
    if (!cls || cls->kind != CK_CLASS)
        vm_raise(vm, ex_class("Error"), op, "Class '%s' not found",
                 tstr_val(cname));
    if (cls->flags & CLS_ABSTRACT)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot instantiate abstract class %s", tstr_val(cls->name));
    if (cls->kind == CK_INTERFACE || cls->kind == CK_TRAIT)
        vm_raise(vm, ex_class("Error"), op, "Cannot instantiate %s",
                 tstr_val(cls->name));
    TObj *o = tphp_obj_alloc(cls);
    o->h.rc++;                    /* ctor invocation holds a reference */
    if (cls->m_ctor >= 0 && !cls->methods[cls->m_ctor].fn &&
        cls->methods[cls->m_ctor].native) {
        /* native constructor (M4 internal classes, e.g. Async\Channel):
         * run the C ctor with the sent args, then land the object */
        NativeMethodFn ctor = (NativeMethodFn)cls->methods[cls->m_ctor].native;
        TVal *args = &vm->x.args[vm->x.n_args - (int)op->a2];
        TVal r = ctor(vm, o, args, (int)op->a2);
        for (uint16_t i = 0; i < op->a2; i++)
            if (args[i].t != T_UNDEF) tval_release(&args[i]);
        vm->x.n_args -= (int)op->a2;
        tval_release(&r);
        if (op->tr != OPD_UNUSED) {
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            *d = TV_OBJ(o);
        } else {
            TVal ov = TV_OBJ(o);
            tval_release(&ov);
        }
        return 1;
    }
    if (cls->m_ctor >= 0 && cls->methods[cls->m_ctor].fn &&
        !cls->methods[cls->m_ctor].is_abstract) {
        VMCallRec *rec = call_push(vm, op);
        rec->is_method = true;
        rec->is_ctor = true;
        rec->this_obj = NULL;      /* $this moves straight into the frame */
        rec->uf = cls->methods[cls->m_ctor].fn;
        rec->dst_kind = op->tr;
        rec->dst_slot = op->ar;
        rec->args_base = (uint32_t)vm->x.n_args - (uint32_t)op->a2;  /* args pre-sent */
        rec->n_args = (uint16_t)op->a2;
        TFunc *fn = rec->uf;
        VMFrame *f = tm_malloc(sizeof *f);
        f->fn = fn;
        f->pc = 0;
        f->prev = vm->x.frame;
        f->args_base = rec->args_base;
        f->n_args_sent = (uint16_t)op->a2;
        f->n_calls_at_entry = vm->x.n_calls;
        uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
        f->slots = tm_malloc(sizeof(TVal) * nslots);
        for (uint32_t i = 0; i < nslots; i++)
            f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
        f->slots[0] = TV_OBJ(o);   /* $this; transfers the +1 reference */
        f->called_cls = cls;
        /* args area winds back to the CALLER's base now that ctor params
         * live in f->args_base (RECV moves from there) — otherwise leftover
         * ctor args pollute the next nested call's parameter area (M5-b:
         * serialize(new Point(3,4)) landed 3 args) */
        vm->x.n_args = rec->args_base;
        vm->x.frame->pc++;           /* caller resumes after NEW_USER */
        vm->x.frame = f;
        return 0;                  /* pc managed: ctor frame is running */
    }
    /* no constructor: the object lands in dst directly */
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_release(d);
        d[0] = TV_OBJ(o);         /* transfer the +1 */
    } else {
        o->h.rc--;
        tval_release(&(TVal){ .u.p = o, .t = T_OBJ });
    }
    return 1;
}

static int32_t op_assign_prop(VM *vm, TOp *op) {
    TVal objv = rdv(vm, op, 1);
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    TVal val = rdv(vm, op, 3);    /* o3: operands are obj, name-lit, value */
    if (objv.t != T_OBJ || ((GCHead *)objv.u.p)->kind != GC_KIND_OBJ)
        vm_raise(vm, ex_class("Error"), op,
                 "Property assignment on non-object lands in M3");
    TObj *o = (TObj *)objv.u.p;
    /* M5.5: FFI\CData 结构体字段写 */
    if (tphp_ffi_assign_prop(vm, o, name, val)) {
        if (op->tr != OPD_UNUSED) {
            tval_addref(val);
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            d[0] = val;
        }
        return 1;
    }
    int slot = tphp_class_find_prop(o->cls, tstr_val(name));
    if (slot >= 0 &&
        !tphp_vis_ok(o->cls->props[slot].visibility, o->cls, frame_scope_cls(vm))) {
        /* invisible from this scope: __set fallback, else error */
        TVal pack[2];
        pack[0] = TV_STR(name);
        tval_addref(pack[0]);
        pack[1] = val;
        tval_addref(pack[1]);
        TVal out;
        if (vm_call_magic(vm, op, o, o->cls, "__set", pack, 2, &out))
            tval_release(&out);
        if (op->tr != OPD_UNUSED) {
            tval_addref(val);
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            d[0] = val;
        }
        return 1;
    }
    if (slot < 0) {
        /* __set(name, value) fallback */
        TVal pack[2];
        pack[0] = TV_STR(name);
        tval_addref(pack[0]);
        pack[1] = val;
        tval_addref(pack[1]);
        TVal out;
        if (vm_call_magic(vm, op, o, o->cls, "__set", pack, 2, &out))
            tval_release(&out);
        if (op->tr != OPD_UNUSED) {
            tval_addref(val);
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            d[0] = val;
        }
        return 1;
    }
    if (o->cls->props[slot].is_readonly && o->props[slot].t != T_UNDEF)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot modify readonly property %s::$%s",
                 tstr_val(o->cls->name), tstr_val(name));
    tval_addref(val);
    tval_release(&o->props[slot]);
    o->props[slot] = val;
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_addref(val);
        tval_release(d);
        d[0] = val;               /* assignment is an expression */
    }
    return 1;
}

static int32_t op_isset_prop(VM *vm, TOp *op) {
    TVal objv = rdv(vm, op, 1);
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    bool set = false;
    if (objv.t == T_OBJ) {
        GCHead *h = (GCHead *)objv.u.p;
        if (h->kind == GC_KIND_OBJ) {
            TObj *o = (TObj *)h;
            int slot = tphp_class_find_prop(o->cls, tstr_val(name));
            if (slot >= 0) {
                if (tphp_vis_ok(o->cls->props[slot].visibility, o->cls,
                                frame_scope_cls(vm)))
                    set = o->props[slot].t != T_UNDEF;
                else if (o->cls->m_isset >= 0) {
                    /* __isset(name) fallback (invisible property) */
                    TVal pack[1];
                    pack[0] = TV_STR(name);
                    tval_addref(pack[0]);
                    TVal out;
                    if (vm_call_magic(vm, op, o, o->cls, "__isset", pack, 1, &out)) {
                        set = out.t == T_TRUE;
                        tval_release(&out);
                    }
                }
            }
            else if (o->cls->m_isset >= 0) {
                /* __isset(name) fallback (overloaded properties) */
                TVal pack[1];
                pack[0] = TV_STR(name);
                tval_addref(pack[0]);
                TVal out;
                if (vm_call_magic(vm, op, o, o->cls, "__isset", pack, 1, &out)) {
                    set = out.t == T_TRUE;
                    tval_release(&out);
                }
            }
        } else if (h->kind == GC_KIND_EX) {
            TExObj *x = (TExObj *)h;
            set = lc_eq(tstr_val(name), "message") ||
                  lc_eq(tstr_val(name), "code") ||
                  lc_eq(tstr_val(name), "previous") ||
                  lc_eq(tstr_val(name), "line");
        }
    }
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_release(d);
        d[0] = set ? TV_TRUE : TV_FALSE;
    }
    return 1;
}

/* resolve a static-call class target: kind 0 self / 1 parent / 2 static /
 * 3 named; late binding uses frame->called_cls (set by frame creators) */
static TClass *smcall_cls(VM *vm, TOp *op, int kind, TStr *named) {
    TClass *decl = (TClass *)vm->x.frame->fn->decl_cls;
    TClass *called = (TClass *)vm->x.frame->called_cls;
    switch (kind) {
    case 0:
        if (!decl) vm_raise(vm, ex_class("Error"), op,
                            "self:: used outside a class context");
        return decl;
    case 1:
        if (!decl || !decl->parent) vm_raise(vm, ex_class("Error"), op,
                            "parent:: used outside a child class context");
        return decl->parent;
    case 2: {
        TClass *c = called ? called : decl;
        if (!c) vm_raise(vm, ex_class("Error"), op,
                         "static:: used outside a class context");
        return c;
    }
    default: {
        char lc[128];
        const char *s = tstr_val(named);
        size_t k = 0;
        for (; s[k] && k < sizeof lc - 1; k++)
            lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
        lc[k] = 0;
        extern TClass *class_resolve_lc(const char *fqcn_lc);
        TClass *c = class_resolve_lc(lc);
        if (!c) vm_raise(vm, ex_class("Error"), op,
                         "Class '%s' not found", s);
        return c;
    }
    }
}

/* shared: called_cls resolution for a frame being created */
static TClass *frame_called_cls(VMCallRec *rec, TFunc *fn) {
    if (rec->is_method)
        return rec->this_obj ? ((TObj *)rec->this_obj)->cls
                             : (TClass *)rec->called_cls;
    return (TClass *)fn->decl_cls;
}

static int32_t op_init_smcall(VM *vm, TOp *op) {
    int kind = (int)op->a1;
    TStr *key = (TStr *)vm->x.frame->fn->lits[op->ar].u.p;
    TClass *cls;
    TStr *mname;
    if (kind == 3) {
        /* "Cls::method": class names may contain backslashes, never colons */
        const char *s = tstr_val(key);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2)
            sep = p;
        if (!sep)
            vm_raise(vm, ex_class("Error"), op, "Malformed static call key");
        char clc[128];
        size_t cl = (size_t)(sep - s);
        if (cl >= sizeof clc) cl = sizeof clc - 1;
        for (size_t i = 0; i < cl; i++)
            clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
        clc[cl] = 0;
        extern TClass *class_resolve_lc(const char *fqcn_lc);
        cls = class_resolve_lc(clc);
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class '%.*s' not found",
                     (int)cl, s);
        mname = tstr_intern(sep + 2, strlen(sep + 2));
    } else {
        cls = smcall_cls(vm, op, kind, NULL);
        mname = key;
    }
    VMCallRec *rec = call_push(vm, op);
    /* internal-class native static method (M5-c): dispatched at DO_FCALL;
     * no receiver */
    {
        TMethod *mnat = tphp_class_find_method(cls, tstr_val(mname));
        if (mnat && !mnat->fn && mnat->native && mnat->is_static) {
            rec->is_builtin = true;
            rec->nm_fn = mnat->native;
            rec->called_cls = cls;
            return 1;
        }
    }
    /* enum built-ins (::from/tryFrom/cases) execute at DO_FCALL time when
     * the arguments are all in the args area */
    TMethod *m = tphp_class_find_method(cls, tstr_val(mname));
    if (cls->kind == CK_ENUM && !m) {
        const char *mn = tstr_val(mname);
        if (lc_eq(mn, "from") || lc_eq(mn, "tryfrom") || lc_eq(mn, "cases")) {
            rec->is_enum_builtin = true;
            rec->is_method = true;
            rec->called_cls = cls;
            return 1;
        }
    }
    if (!m || !m->fn || m->is_abstract) {
        TMethod *mcs = tphp_class_find_method(cls, "__callstatic");
        if (mcs && mcs->fn && !mcs->is_abstract) {
            /* __callStatic(name, args): defer to DO_FCALL so the real
             * argument list can be packed (PHP signature semantics) */
            rec->magic_kind = 2;
            rec->magic_name = mname;
            rec->is_method = true;
            rec->called_cls = cls;
            return 1;
        }
        vm_raise(vm, ex_class("Error"), op,
                 "Call to undefined static method %s::%s()",
                 tstr_val(cls->name), tstr_val(mname));
    }
    if (lc_eq(tstr_val(mname), "__construct")) {
        /* parent::__construct() / self::__construct(): the one legal
         * "static" call to a non-static method — run the ctor body on the
         * current $this (caller slot 0); expression result is NULL */
        rec->is_method = true;
        rec->called_cls = cls;
        rec->uf = m->fn;
        if (vm->x.frame->slots[0].t == T_OBJ) {
            TObj *o = (TObj *)vm->x.frame->slots[0].u.p;
            o->h.rc++;               /* reference moves into the ctor frame */
            rec->this_obj = o;
        }
        return 1;
    }
    if (!m->is_static) {
        /* self::/parent::/static:: forward to the instance method on the
         * current $this (the one legal non-static "static" call besides
         * __construct); an explicit Cls::method() stays an error */
        if (kind != 3 && vm->x.frame->slots[0].t == T_OBJ) {
            TObj *o = (TObj *)vm->x.frame->slots[0].u.p;
            o->h.rc++;               /* reference moves into the new frame */
            rec->is_method = true;
            rec->called_cls = cls;
            rec->this_obj = o;
            rec->uf = m->fn;
            return 1;
        }
        vm_raise(vm, ex_class("Error"), op,
                 "Non-static method %s::%s() called statically",
                 tstr_val(cls->name), tstr_val(mname));
    }
    if (!tphp_vis_ok(m->visibility, cls, frame_scope_cls(vm))) {
        TMethod *mcs = tphp_class_find_method(cls, "__callstatic");
        if (mcs && mcs->fn && !mcs->is_abstract) {
            rec->magic_kind = 2;
            rec->magic_name = mname;
            rec->is_method = true;
            rec->called_cls = cls;
            return 1;
        }
        vm_raise(vm, ex_class("Error"), op,
                 "Call to %s method %s::%s() from scope %s",
                 vis_name(m->visibility), tstr_val(cls->name), tstr_val(mname),
                 frame_scope_cls(vm) ? tstr_val(frame_scope_cls(vm)->name) : "global");
    }
    rec->is_method = true;
    rec->called_cls = cls;
    rec->uf = m->fn;
    return 1;
}

static int32_t op_fetch_cconst(VM *vm, TOp *op) {
    int kind = (int)op->a1;
    TStr *key = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    TClass *cls;
    TStr *cname;
    if (kind == 3) {
        /* "Cls::member": split on the separator (class names may contain
         * backslashes but never colons) */
        const char *s = tstr_val(key);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2)
            sep = p;
        if (!sep)
            vm_raise(vm, ex_class("Error"), op, "Malformed class constant key");
        char clc[128];
        size_t cl = (size_t)(sep - s);
        if (cl >= sizeof clc) cl = sizeof clc - 1;
        for (size_t i = 0; i < cl; i++)
            clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
        clc[cl] = 0;
        extern TClass *class_resolve_lc(const char *fqcn_lc);
        cls = class_resolve_lc(clc);
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class '%.*s' not found",
                     (int)cl, s);
        cname = tstr_intern(sep + 2, strlen(sep + 2));
    } else {
        cls = smcall_cls(vm, op, kind, NULL);
        cname = key;
    }
    tphp_class_ensure_flat(cls);   /* constants/enum cases/sprops need the flat view */
    /* self::class / static::class / parent::class: FQ class name
     * (smcall_cls already resolves parent to the parent class) */
    if (tstr_case_eq(tstr_val(cname), "class")) {
        wtake(vm, op, TV_STR(cls->name));
        return 1;
    }
    TVal out;
    if (tstr_val(cname)[0] == '$') {
        /* C::$prop static property read (parse keeps the leading '$') */
        TProp *sp = tphp_class_find_sprop(cls, tstr_val(cname) + 1);
        if (!sp)
            vm_raise(vm, ex_class("Error"), op,
                     "Access to undeclared static property %s::$%s",
                     tstr_val(cls->name), tstr_val(cname) + 1);
        if (!tphp_vis_ok(sp->visibility, cls, frame_scope_cls(vm)))
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot access %s property %s::$%s",
                     vis_name(sp->visibility), tstr_val(cls->name),
                     tstr_val(cname) + 1);
        TVal r = tphp_class_sprop_vals(cls)[sp->slot];
        tval_addref(r);
        wtake(vm, op, r);
        return 1;
    }
    if (tphp_class_has_const(cls, tstr_val(cname), &out)) {
        tval_addref(out);
        wtake(vm, op, out);
        return 1;
    }
    /* enum case access: Status::Active */
    if (cls->kind == CK_ENUM) {
        for (int i = 0; i < cls->n_cases; i++) {
            if (tstr_case_eq(tstr_val(cls->cases[i].name), tstr_val(cname))) {
                TVal r = TV_OBJ(tphp_enum_case_obj(cls, i));
                wtake(vm, op, r);
                return 1;
            }
        }
    }
    vm_raise(vm, ex_class("Error"), op,
             "Undefined constant %s::%s",
             tstr_val(cls->name), tstr_val(cname));
}

/* static property write: C::$prop = v / self::$prop = v
 * operands: o1=kind (0 self/1 parent/2 static/3 FQ), o2=key lit, o3=value */
static int32_t op_assign_sprop(VM *vm, TOp *op) {
    int kind = (int)op->a1;
    TStr *key = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    TClass *cls;
    TStr *pname;
    if (kind == 3) {
        const char *s = tstr_val(key);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2)
            sep = p;
        if (!sep)
            vm_raise(vm, ex_class("Error"), op, "Malformed static property key");
        char clc[128];
        size_t cl = (size_t)(sep - s);
        if (cl >= sizeof clc) cl = sizeof clc - 1;
        for (size_t i = 0; i < cl; i++)
            clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
        clc[cl] = 0;
        extern TClass *class_resolve_lc(const char *fqcn_lc);
        cls = class_resolve_lc(clc);
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class '%.*s' not found",
                     (int)cl, s);
        pname = tstr_intern(sep + 2, strlen(sep + 2));
    } else {
        cls = smcall_cls(vm, op, kind, NULL);
        pname = key;
    }
    if (tstr_val(pname)[0] != '$')
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot assign to constant %s::%s",
                 tstr_val(cls->name), tstr_val(pname));
    TProp *sp = tphp_class_find_sprop(cls, tstr_val(pname) + 1);
    if (!sp)
        vm_raise(vm, ex_class("Error"), op,
                 "Access to undeclared static property %s::$%s",
                 tstr_val(cls->name), tstr_val(pname) + 1);
    if (!tphp_vis_ok(sp->visibility, cls, frame_scope_cls(vm)))
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot access %s property %s::$%s",
                 vis_name(sp->visibility), tstr_val(cls->name),
                 tstr_val(pname) + 1);
    TVal val = rdv(vm, op, 3);
    tval_addref(val);
    TVal *slot = &tphp_class_sprop_vals(cls)[sp->slot];
    tval_release(slot);
    *slot = val;
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_addref(val);
        tval_release(d);
        d[0] = val;               /* assignment is an expression */
    }
    return 1;
}

/* unset($obj->prop): a declared slot becomes UNDEF (isset flips false);
 * a missing slot forwards to __unset(name) */
static int32_t op_unset_prop(VM *vm, TOp *op) {
    TVal objv = rdv(vm, op, 1);
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    if (objv.t != T_OBJ || ((GCHead *)objv.u.p)->kind != GC_KIND_OBJ)
        return 1;                         /* non-user objects: no-op */
    TObj *o = (TObj *)objv.u.p;
    int slot = tphp_class_find_prop(o->cls, tstr_val(name));
    if (slot >= 0) {
        if (!tphp_vis_ok(o->cls->props[slot].visibility, o->cls, frame_scope_cls(vm))) {
            /* invisible: __unset fallback, else error */
            TVal pack[1];
            pack[0] = TV_STR(name);
            tval_addref(pack[0]);
            TVal out;
            if (vm_call_magic(vm, op, o, o->cls, "__unset", pack, 1, &out))
                tval_release(&out);
            else
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot access %s property %s::$%s",
                         vis_name(o->cls->props[slot].visibility),
                         tstr_val(o->cls->name), tstr_val(name));
            return 1;
        }
        tval_release(&o->props[slot]);
        o->props[slot] = (TVal){ .u.i = 0, .t = T_UNDEF };
        return 1;
    }
    TVal pack[1];
    pack[0] = TV_STR(name);
    tval_addref(pack[0]);
    TVal out;
    if (vm_call_magic(vm, op, o, o->cls, "__unset", pack, 1, &out))
        tval_release(&out);
    return 1;
}

/* clone $obj: fresh TObj, shallow-copied property slots (+1 each), then the
 * __clone hook runs on the copy (PHP order: copy first, hook second) */
static int32_t op_clone(VM *vm, TOp *op) {
    TVal srcv = rdv(vm, op, 1);
    if (srcv.t != T_OBJ || ((GCHead *)srcv.u.p)->kind != GC_KIND_OBJ)
        vm_raise(vm, ex_class("Error"), op, "Cannot clone a non-object value");
    TObj *src = (TObj *)srcv.u.p;
    TObj *cp = tphp_obj_alloc(src->cls);
    for (int i = 0; i < src->cls->n_props; i++) {
        TVal v = src->props[i];
        tval_addref(v);
        cp->props[i] = v;
    }
    TVal cv = TV_OBJ(cp);
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_release(d);
        *d = cv;                      /* the slot now owns the +1 */
    } else {
        tval_release(&cv);            /* result dropped: the hook re-addrefs */
    }
    if (cp->cls->m_clone >= 0) {
        TMethod *m = &cp->cls->methods[cp->cls->m_clone];
        if (m->fn && !m->is_abstract) {
            static TOp stub;          /* raise-site stub: line 0 */
            memset(&stub, 0, sizeof stub);
            TVal out;
            vm_call_magic(vm, &stub, cp, cp->cls, "__clone", NULL, 0, &out);
            tval_release(&out);
        }
    }
    return 1;
}

/* tval_to_str hook: T_OBJ converts through __toString(); the method must
 * return a string (PHP 8 raises otherwise) */
static TStr *vm_tostring_hook(TVal v) {
    VM *vm = (VM *)tphp_vm_current;
    if (!vm)
        tphp_raise("TypeError", "Object converted outside the VM");
    GCHead *h = (GCHead *)v.u.p;
    if (h->kind != GC_KIND_OBJ)
        tphp_raise("Error", "Exceptions cannot be converted to string");
    TObj *o = (TObj *)h;
    if (o->cls->m_tostring < 0)
        tphp_raise("Error", "Object of class %s could not be converted to string",
                   tstr_val(o->cls->name));
    static TOp stub;
    memset(&stub, 0, sizeof stub);
    TVal out;
    if (!vm_call_magic(vm, &stub, o, o->cls, "__toString", NULL, 0, &out))
        tphp_raise("Error", "Object of class %s could not be converted to string",
                   tstr_val(o->cls->name));
    if (out.t != T_STR) {
        tval_release(&out);
        tphp_raise("TypeError", "__toString() must return a string value");
    }
    return (TStr *)out.u.p;           /* move the +1 string to the caller */
}

/* expr::class: objects -> the class FQ name; class-name strings pass
 * through unchanged (PHP 8.0 $obj::class / 8.2 Status::Active::class) */
static int32_t op_get_class(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    if (v.t == T_STR) {
        tval_addref(v);
        wtake(vm, op, v);
        return 1;
    }
    if (v.t != T_OBJ || ((GCHead *)v.u.p)->kind != GC_KIND_OBJ)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot get class of a non-object value");
    TClass *cls = ((TObj *)v.u.p)->cls;
    wtake(vm, op, TV_STR(cls->name));
    return 1;
}

static int32_t op_instanceof(VM *vm, TOp *op) {
    TVal l = rdv(vm, op, 1);
    TStr *cname = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    bool res = false;
    if (l.t == T_OBJ) {
        GCHead *h = (GCHead *)l.u.p;
        if (h->kind == GC_KIND_OBJ) {
            char lc[128];
            const char *s = tstr_val(cname);
            size_t k = 0;
            for (; s[k] && k < sizeof lc - 1; k++)
                lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
            lc[k] = 0;
            extern TClass *class_resolve_lc(const char *fqcn_lc);
            TClass *b = class_resolve_lc(lc);
            if (b) res = tphp_class_isa(((TObj *)h)->cls, b);
        } else if (h->kind == GC_KIND_EX) {
            /* exception objects: match against the built-in hierarchy */
            res = lc_eq(tstr_val(cname), ((TExObj *)h)->cls->name);
            for (const TExClass *c = ((TExObj *)h)->cls->parent;
                 !res && c; c = c->parent)
                res = lc_eq(tstr_val(cname), c->name);
        }
    }
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_release(d);
        d[0] = res ? TV_TRUE : TV_FALSE;
    }
    return 1;
}

static int32_t op_fetch_prop(VM *vm, TOp *op) {
    TVal obj = rdv(vm, op, 1);
    TStr *name = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    if (obj.t != T_OBJ)
        vm_raise(vm, ex_class("Error"), op,
                 "Attempt to read property \"%s\" on %s",
                 tstr_val(name),
                 obj.t == T_NULL ? "null" :
                 obj.t == T_STR ? "string" :
                 obj.t == T_INT ? "int" :
                 obj.t == T_FLOAT ? "float" :
                 obj.t == T_ARR ? "array" : "bool");
    GCHead *h = (GCHead *)obj.u.p;
    if (h->kind == GC_KIND_OBJ) {
        TObj *o = (TObj *)h;
        /* M5.5: FFI\CData 结构体字段 */
        {
            TVal fv;
            if (tphp_ffi_fetch_prop(vm, o, name, &fv)) {
                wtake(vm, op, fv);
                return 1;
            }
        }
        int slot = tphp_class_find_prop(o->cls, tstr_val(name));
        if (slot < 0) {
            /* enum case introspection: ->value / ->name */
            if (o->cls->kind == CK_ENUM) {
                int ci = tphp_enum_case_find(o->cls, o);
                if (ci >= 0) {
                    if (lc_eq(tstr_val(name), "value")) {
                        if (!o->cls->backed)
                            vm_raise(vm, ex_class("Error"), op,
                                     "Enum case has no value (unit enum)");
                        TVal out = o->cls->cases[ci].backing;
                        tval_addref(out);
                        wtake(vm, op, out);
                        return 1;
                    }
                    if (lc_eq(tstr_val(name), "name")) {
                        TVal out = TV_STR(o->cls->cases[ci].name);
                        tval_addref(out);
                        wtake(vm, op, out);
                        return 1;
                    }
                }
            }
            /* __get(name) fallback */
            TVal pack[1];
            pack[0] = TV_STR(name);
            tval_addref(pack[0]);
            TVal out;
            if (vm_call_magic(vm, op, o, o->cls, "__get", pack, 1, &out)) {
                wtake(vm, op, out);
                return 1;
            }
            vm_raise(vm, ex_class("UndefinedError"), op,
                     "Undefined property %s::$%s",
                     tstr_val(o->cls->name), tstr_val(name));
        }
        if (o->props[slot].t == T_UNDEF)
            vm_raise(vm, ex_class("UndefinedError"), op,
                     "Typed property %s::$%s must not be accessed before initialization",
                     tstr_val(o->cls->name), tstr_val(name));
        if (!tphp_vis_ok(o->cls->props[slot].visibility, o->cls, frame_scope_cls(vm))) {
            /* invisible from this scope: __get fallback, else error */
            TVal pack[1];
            pack[0] = TV_STR(name);
            tval_addref(pack[0]);
            TVal out;
            if (vm_call_magic(vm, op, o, o->cls, "__get", pack, 1, &out)) {
                wtake(vm, op, out);
                return 1;
            }
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot access %s property %s::$%s",
                     vis_name(o->cls->props[slot].visibility),
                     tstr_val(o->cls->name), tstr_val(name));
        }
        TVal out = o->props[slot];
        tval_addref(out);
        wtake(vm, op, out);
        return 1;
    }
    if (h->kind != GC_KIND_EX)
        vm_raise(vm, ex_class("Error"), op,
                 "Property access on non-object lands in M3");
    TExObj *x = (TExObj *)obj.u.p;
    const char *n = tstr_val(name);
    if (strcmp(n, "message") == 0) {
        TVal out = TV_STR(x->message);
        tval_addref(out);
        wtake(vm, op, out);
        return 1;
    }
    if (strcmp(n, "code") == 0) {
        wtake(vm, op, TV_INT(x->code));
        return 1;
    }
    if (strcmp(n, "previous") == 0) {
        TVal out = x->previous ? TV_OBJ(x->previous) : TV_NULL;
        tval_addref(out);
        wtake(vm, op, out);
        return 1;
    }
    if (strcmp(n, "line") == 0) {
        wtake(vm, op, TV_INT(x->line));
        return 1;
    }
    vm_raise(vm, ex_class("Error"), op,
             "Undefined property: %s (full objects land in M3)", n);
}

static int32_t op_rethrow(VM *vm, TOp *op) {
    if (!vm->x.exception)
        vm_raise(vm, ex_class("Error"), op, "Internal: rethrow without exception");
    longjmp(vm->x.unwind, 1);
}

/* ---- unwinding ---- */
static bool catch_matches(const TExEntry *e, const TExObj *ex) {
    for (int i = 0; i < e->n_catches; i++) {
        const TExClass *want = tphp_ex_classes[e->catch_cls[i]];
        for (const TExClass *c = ex->cls; c; c = c->parent)
            if (c == want) return true;
    }
    return false;
}

static void frame_destroy(VMFrame *f) {
    TFunc *fn = f->fn;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    for (uint32_t i = 0; i < nslots; i++)
        tval_release(&f->slots[i]);
    tm_free(f->slots);
    tm_free(f);
}

/* unwind after a throw: find a handler or pop frames; ends Uncaught at the
 * bottom with a PHP-style stack trace */
static void vm_unwind(VM *vm) {
    TExObj *ex = vm->x.exception;
    vm->x.n_trace = 0;
    while (vm->x.frame) {
        VMFrame *f = vm->x.frame;
        TFunc *fn = f->fn;
        if (vm->x.n_trace < 64 && fn->n_ops && f->pc < fn->n_ops) {
            vm->x.trace[vm->x.n_trace].fn =
                fn->name ? tstr_val(fn->name) : "{closure}";
            vm->x.trace[vm->x.n_trace].line = fn->ops[f->pc].line;
            vm->x.n_trace++;
        }
        /* innermost try region first; within it, catch clauses in
         * declaration order (multiple catch blocks share the region) */
        uint32_t best_end = UINT32_MAX;
        for (int i = 0; i < fn->n_ex; i++) {
            TExEntry *e = &fn->ex[i];
            if (f->pc >= e->start_pc && f->pc < e->end_pc &&
                e->end_pc < best_end)
                best_end = e->end_pc;
        }
        TExEntry *hit = NULL;
        if (best_end != UINT32_MAX) {
            for (int i = 0; i < fn->n_ex; i++) {
                TExEntry *e = &fn->ex[i];
                if (f->pc >= e->start_pc && f->pc < e->end_pc &&
                    e->end_pc == best_end && catch_matches(e, ex)) {
                    hit = e;
                    break;
                }
            }
        }
        if (hit) {
            f->pc = hit->handler_pc;   /* handler starts with BIND_EX if any */
            /* resume in the SAME frame: drop call recs opened inside it
             * (builtin/magic/thunk raises leave their rec on the stack —
             * M4-c cont_check_cancel fixed the cancel path, this covers
             * plain raises) and wind args back to the frame's base */
            vm->x.n_calls = f->n_calls_at_entry;
            vm->x.n_args = (int)f->args_base;
            return;                    /* resume with vm->x.exception set */
        }
        /* no matching handler in this frame: pop it. at_entry still holds
         * this frame's own call rec (main/coroutine-root frames have none) */
        vm->x.n_calls = f->n_calls_at_entry;
        if (vm->x.n_calls > 0) vm->x.n_calls--;
        vm->x.n_args = (int)f->args_base;
        vm->x.frame = f->prev;
        frame_destroy(f);
    }
    /* bottom reached: uncaught。main 协程就地打印 fatal 并定退出码；
     * 子协程（coro_result != NULL）静默收尾——异常保留在执行上下文，
     * 由 await 透传，或在脚本结束时由调度器统一报 fatal（M4 sched.c） */
    vm->x.done = true;
    vm->x.uncaught = true;
    if (!vm->x.coro_result) {
        const char *file = vm->prog->file ? vm->prog->file : "-";
        fprintf(stderr, "PHP Fatal error:  Uncaught %s: %s in %s:%d\n",
                ex->cls->name, tstr_val(ex->message), file, ex->line);
        fprintf(stderr, "Stack trace:\n");
        for (int i = vm->x.n_trace - 1; i >= 0; i--)
            fprintf(stderr, "#%d %s(%d)\n", vm->x.n_trace - 1 - i,
                    vm->x.trace[i].fn, vm->x.trace[i].line);
        fprintf(stderr, "  thrown in %s on line %d\n", file, ex->line);
        fflush(stdout);
        vm->exit_code = 255;
    }
}

/* ---- thunk machinery (runtime default values) ----
 * A zero-arg compiled function runs to completion on top of the current
 * frame; its RETURN stores into vm->x.thunk_ret (rec->is_thunk). */
TVal tphp_call_thunk(VM *vm, uint16_t fn_idx) {
    TFunc *fn = vm->prog->funcs[fn_idx];
    if (vm->x.n_calls >= VM_MAX_CALLS)
        vm_raise(vm, ex_class("Error"), NULL, "Call stack overflow");
    VMCallRec *rec = &vm->x.calls[vm->x.n_calls++];
    memset(rec, 0, sizeof *rec);
    rec->uf = fn;
    rec->args_base = (uint32_t)vm->x.n_args;
    rec->is_thunk = true;
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = fn;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = (uint32_t)vm->x.n_args;
    f->n_args_sent = 0;
    f->n_calls_at_entry = vm->x.n_calls;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    /* caller pc is NOT advanced here: thunks are always invoked from
     * inside a handler (op_recv / op_new_user->tphp_obj_alloc), and the
     * dispatch loop advances pc by that handler's return value. */
    vm->x.frame = f;
    static int thunk_trace = -1;
    if (thunk_trace < 0) thunk_trace = getenv("TPHP_TRACE") ? 1 : 0;
    while (vm->x.frame == f) {
        TOp *op = &f->fn->ops[f->pc];
        if (thunk_trace) fprintf(stderr, "[trace %s pc=%u %s]\n", tstr_val(f->fn->name),
                                 f->pc, tphp_op_name(op->op));
        int32_t d = ((OpHandler)op->handler)(vm, op);
        if (d) f->pc += (uint32_t)d;
    }
    return vm->x.thunk_ret;
}

/* ---- handler table ---- */
#define H(f) ((OpHandler)(f))

const OpHandler tphp_op_handlers[OP_MAX] = {
    H(op_const), H(op_copy),                                /* CONST COPY */
    H(op_add), H(op_sub), H(op_mul), H(op_div), H(op_mod), H(op_pow),
    H(op_concat), H(op_bw_and), H(op_bw_or), H(op_bw_xor), H(op_shl), H(op_shr),
    H(op_seq), H(op_sne), H(op_eq), H(op_ne), H(op_cmp), H(op_lt), H(op_le), H(op_bool_not),
    H(op_jmp), H(op_jmpz), H(op_jmpnz),
    H(op_fetch_dim_r), H(op_fetch_dim_is), H(op_fetch_global),
    H(op_copy), H(op_assign_dim), H(op_assign_global),      /* ASSIGN = COPY */
    H(op_init_fcall), H(op_init_dyn), H(op_send_val), H(op_send_val), H(op_recv),
    H(op_do_fcall), H(op_return),
    H(op_array_init), H(op_array_append), H(op_array_spread),
    H(op_fe_reset), H(op_fe_fetch),
    H(op_isset_dim), H(op_isset_cv), H(op_inc), H(op_dec),
    H(op_isset_global), H(op_unset_global), H(op_unset_cv), H(op_unset_dim),
    H(op_bool_val), H(op_cast), H(op_neg), H(op_pos), H(op_bit_not),
    H(op_jmp_null), H(op_define_const), H(op_fetch_const),
    H(op_send_cap), H(op_closure_init), H(op_get_func),
    H(op_send_spread), H(op_recv_variadic),
    H(op_new_ex), H(op_throw), H(op_bind_ex), H(op_rethrow), H(op_fetch_prop),
    H(op_fcc_dyn),
    H(op_init_mcall), H(op_new_user), H(op_assign_prop), H(op_isset_prop),
    H(op_init_smcall), H(op_fetch_cconst), H(op_instanceof), H(op_get_class),
    H(op_assign_sprop), H(op_unset_prop), H(op_clone),
};

const char *tphp_op_name(uint8_t op) {
    static const char *names[] = {
        "CONST", "COPY", "ADD", "SUB", "MUL", "DIV", "MOD", "POW",
        "CONCAT", "BW_AND", "BW_OR", "BW_XOR", "SHL", "SHR",
        "SEQ", "SNE", "EQ", "NE", "CMP", "LT", "LE", "BOOL_NOT",
        "JMP", "JMPZ", "JMPNZ",
        "FETCH_DIM_R", "FETCH_DIM_IS", "FETCH_GLOBAL",
        "ASSIGN", "ASSIGN_DIM", "ASSIGN_GLOBAL",
        "INIT_FCALL", "INIT_DYN", "SEND_VAL", "SEND_VAR", "RECV",
        "DO_FCALL", "RETURN",
        "ARRAY_INIT", "ARRAY_APPEND", "ARRAY_SPREAD",
        "FE_RESET", "FE_FETCH",
        "ISSET_DIM", "ISSET_CV", "INC", "DEC",
        "ISSET_GLOBAL", "UNSET_GLOBAL", "UNSET_CV", "UNSET_DIM",
        "BOOL_VAL", "CAST", "NEG", "POS", "BIT_NOT",
        "JMP_NULL", "DEFINE_CONST", "FETCH_CONST",
        "SEND_CAP", "CLOSURE_INIT", "GET_FUNC",
        "SEND_SPREAD", "RECV_VARIADIC",
        "NEW_EX", "THROW", "BIND_EX", "RETHROW", "FETCH_PROP",
        "FCC_DYN",
        "INIT_MCALL", "NEW_USER", "ASSIGN_PROP", "ISSET_PROP",
        "INIT_SMCALL", "FETCH_CCONST", "INSTANCEOF", "GET_CLASS",
        "ASSIGN_SPROP", "UNSET_PROP", "CLONE",
    };
    return op < OP_MAX ? names[op] : "?";
}

/* creation-order object id (var_dump object#N), bumped by tphp_obj_alloc */
uint32_t _Thread_local tphp_obj_next_id = 1;

/* run a PHP-level autoloader (TClosure / function-name string) with one
 * string argument. Silent no-op outside the VM (load time) or on an
 * unresolvable callable; exceptions propagate through the normal unwind. */
void tphp_vm_run_autoloader(TVal fn, TStr *name) {
    VM *vm = (VM *)tphp_vm_current;
    if (!vm) return;
    TClosure *clo = NULL;
    TFunc *uf = NULL;
    BuiltinFn bf = NULL;
    if (fn.t == T_CLOSURE) {
        clo = (TClosure *)fn.u.p;
    } else if (fn.t == T_STR) {
        bool isb;
        void *hit = resolve_fn(vm, (TStr *)fn.u.p, &isb);
        if (!hit) return;
        if (isb) bf = (BuiltinFn)hit; else uf = (TFunc *)hit;
    } else {
        return;
    }
    /* queue the single argument (class name), ownership moves along */
    uint32_t base = (uint32_t)vm->x.n_args;
    tval_addref(TV_STR(name));
    vm->x.args[base] = TV_STR(name);
    vm->x.n_args = (int)base + 1;
    VMCallRec *rec = call_push(vm, &((TOp){0}));
    rec->line = 0;
    if (clo) rec->clo = clo;
    else if (bf) { rec->is_builtin = true; rec->bf = bf; }
    else rec->uf = uf;
    TFunc *fnc = clo ? clo->fn : uf;
    if (!fnc) {                       /* builtin: run directly */
        BuiltinFn b = rec->bf;
        TVal r = b(vm, &vm->x.args[base], 1);
        tval_release(&r);
        if (vm->x.args[base].t != T_UNDEF) tval_release(&vm->x.args[base]);
        vm->x.n_args = (int)base;
        if (vm->x.exception) longjmp(vm->x.unwind, 1);
        return;
    }
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = fnc;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = base;
    f->n_args_sent = 1;
    f->called_cls = NULL;         /* no class scope for autoloaders */
    f->n_calls_at_entry = vm->x.n_calls;
    uint32_t nslots = (uint32_t)fnc->n_locals + fnc->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    if (clo)
        for (uint16_t i = 0; i < clo->n_cap; i++) {
            tval_addref(clo->captures[i]);
            f->slots[i] = clo->captures[i];
        }
    vm->x.frame = f;
    while (vm->x.frame == f) {
        TOp *cop = &f->fn->ops[f->pc];
        int32_t d = ((OpHandler)cop->handler)(vm, cop);
        if (d) f->pc += (uint32_t)d;
    }
    if (vm->x.exception) longjmp(vm->x.unwind, 1);
}

/* __debugInfo() dispatch for var_dump: returns the method's array (+1 ref),
 * or TV_NULL when the class does not define __debugInfo */
TVal tphp_vm_debug_info(TObj *o) {
    VM *vm = (VM *)tphp_vm_current;
    if (!vm || o->cls->m_debuginfo < 0) return TV_NULL;
    TMethod *m = &o->cls->methods[o->cls->m_debuginfo];
    if (!m->fn || m->is_abstract) return TV_NULL;
    static TOp stub;              /* raise-site stub: line 0 */
    memset(&stub, 0, sizeof stub);
    TVal out;
    if (!vm_call_magic(vm, &stub, o, o->cls, "__debugInfo", NULL, 0, &out))
        return TV_NULL;
    if (out.t != T_ARR) {         /* PHP 8 raises TypeError; keep it simple */
        tval_release(&out);
        return TV_NULL;
    }
    return out;
}

/* ---- entry (M4: scheduler-driven coroutines) ---- */

/* per-context heap arrays: allocated once, owned by the coroutine's
 * parked TExecCtx while it is suspended */
void tphp_ctx_ensure(TExecCtx *x) {
    if (!x->calls) {
        x->calls = tm_malloc(sizeof(VMCallRec) * VM_MAX_CALLS);
        memset(x->calls, 0, sizeof(VMCallRec) * VM_MAX_CALLS);
        x->n_calls = 0;
    }
    if (!x->args) {
        x->args = tm_malloc(sizeof(TVal) * VM_MAX_ARGS);
        memset(x->args, 0, sizeof(TVal) * VM_MAX_ARGS);   /* T_UNDEF */
        x->n_args = 0;
    }
    if (!x->caps) {
        x->caps = tm_malloc(sizeof(TVal) * 256);
        memset(x->caps, 0, sizeof(TVal) * 256);
        x->n_caps = 0;
    }
}

/* the single VM instance: static storage so longjmp/fiber switches stay
 * valid; sched.c reaches it via tphp_vm_instance() */
static _Thread_local VM g_vm;
VM *tphp_vm_instance(void) { return &g_vm; }

/* run the current coroutine's frames until done. Must be called on the
 * coroutine's own fiber (setjmp/longjmp never cross a fiber switch). */
static void vm_run_until_done(VM *vm) {
    static int trace = -1;
    if (trace < 0) trace = getenv("TPHP_TRACE") ? 1 : 0;
    while (!vm->x.done) {
        if (setjmp(vm->x.unwind) != 0) {
            vm_unwind(vm);        /* may resume in a handler or finish */
            continue;
        }
        while (!vm->x.done) {
            VMFrame *fr = vm->x.frame;
            TOp *op = &fr->fn->ops[fr->pc];
            if (trace)
                fprintf(stderr, "[trace %s pc=%u %s]\n", tstr_val(fr->fn->name),
                        fr->pc, tphp_op_name(op->op));
            int32_t d = ((OpHandler)op->handler)(vm, op);
            if (d) fr->pc += (uint32_t)d;
        }
        break;
    }
}

/* sched.c: main/协程体入口 —— 外圈 setjmp + 跑帧直到 done。
 * 内圈（vm_run_until_done）的 setjmp 会覆盖这里；外圈落地只可能是
 * 协程根帧构建期的 raise，此时就地展开到底。 */
void tphp_vm_run_current(void) {
    if (setjmp(g_vm.x.unwind) == 0)
        vm_run_until_done(&g_vm);
    else
        vm_unwind(&g_vm);
}

/* sched.c: 外圈 setjmp 落地时展开当前协程（协程根帧构建期的 raise） */
void tphp_vm_unwind_current(void) {
    vm_unwind(&g_vm);
}

/* 预置内置常量（M4：I/O 相关；define() 运行期走 op_define_const） */
static void vm_seed_consts(VM *vm) {
    static const struct { const char *name; TVal v; } seeds[] = {
        { "FILE_APPEND", { .u.i = 8,  .t = T_INT } },
        { "LOCK_EX",     { .u.i = 2,  .t = T_INT } },
        { "SEEK_SET",    { .u.i = 0,  .t = T_INT } },
        { "SEEK_CUR",    { .u.i = 1,  .t = T_INT } },
        { "SEEK_END",    { .u.i = 2,  .t = T_INT } },
    };
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        if (vm->n_consts >= 256) return;
        vm->consts[vm->n_consts].name = tstr_new(seeds[i].name,
                                                 strlen(seeds[i].name));
        vm->consts[vm->n_consts].v = seeds[i].v;
        vm->n_consts++;
    }
}

/* ---- __cloneState protocol (M5-b: serialize / cross-thread cloning) ----
 * 导出：__cloneState(): array（用户定义对象状态快照）。
 * 恢复：__restoreCloneState(array $state): void 优先；未实现则把状态数组的
 * string 键逐一同名属性槽直填（内部机制，绕可见性；缺属性忽略）。 */

/* stub op for magic calls from non-opcode contexts (line 0) */
static _Thread_local TOp g_magic_stub;

/* 调 __cloneState()；类无此方法返回 false（调用方抛 NotCloneableError）。
 * 结果 +1 引用。 */
bool tphp_vm_clone_state(VM *vm, TObj *o, TVal *out) {
    memset(&g_magic_stub, 0, sizeof g_magic_stub);
    if (!vm_call_magic(vm, &g_magic_stub, o, o->cls, "__cloneState",
                       NULL, 0, out))
        return false;
    return true;
}

void tphp_vm_restore_clone_state(VM *vm, TObj *o, TVal state) {
    memset(&g_magic_stub, 0, sizeof g_magic_stub);
    if (tphp_class_find_method(o->cls, "__restoreclonestate")) {
        TVal args[1] = { state };
        tval_addref(state);
        TVal out;
        vm_call_magic(vm, &g_magic_stub, o, o->cls, "__restoreCloneState",
                      args, 1, &out);
        return;
    }
    /* 默认恢复：状态数组 string 键 → 同名属性槽直填 */
    if (state.t != T_ARR) return;
    TArr *a = (TArr *)state.u.p;
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;
        /* 反查键（data 序 ↔ bucket） */
        TVal key = (TVal){ .u.i = 0, .t = T_NULL };
        for (uint32_t b = 0; b < a->bcap; b++) {
            if (a->buckets[b].val_off == i) { key = a->buckets[b].key; break; }
        }
        if (key.t != T_STR) continue;
        int slot = tphp_class_find_prop(o->cls, tstr_val((TStr *)key.u.p));
        if (slot < 0) continue;
        TVal val = a->data[i];
        tval_addref(val);
        tval_release(&o->props[slot]);
        o->props[slot] = val;
    }
}

/* ---- prog freeze (M5): after compilation, before any thread runs ----
 * 1) eager-flatten every class: runtime ensure_flat calls become no-ops,
 *    so the shared TClass structures are never mutated while workers read
 *    them; static-prop templates are seeded once (side tables copy them)
 * 2) pre-resolve INIT_FCALL inline caches: op->ic becomes a process-level
 *    constant (TFunc* / builtin fn), so worker threads run the same prog
 *    without writing to shared bytecode */
void tphp_prog_freeze(Program *p) {
    tphp_class_flatten_all();
    VM *vm = &g_vm;
    vm->prog = p;
    for (int fi = -1; fi < p->n_funcs; fi++) {
        TFunc *fn = fi < 0 ? p->main : p->funcs[fi];
        for (uint32_t k = 0; k < fn->n_ops; k++) {
            TOp *op = &fn->ops[k];
            if (op->op != OP_INIT_FCALL || op->ic) continue;
            TStr *name = (TStr *)fn->lits[op->a1].u.p;
            bool is_builtin;
            void *hit = resolve_fn_fb(vm, op, name, &is_builtin);
            if (hit)
                op->ic = is_builtin ? (void *)((uintptr_t)hit | 1) : hit;
            /* unresolved stays NULL: the runtime slow path reports it */
        }
    }
}

/* 调用任意 PHP callable（M5-c：worker 消息回调）：
 * closure（含 first-class builtin 包装）或函数名字符串。返回 +1 值；
 * 异常沿正常 unwind 传播。 */
TVal tphp_vm_call_callable(VM *vm, TVal callable, TVal *args, int n) {
    TClosure *clo = NULL;
    BuiltinFn bfn = NULL;
    TFunc *uf = NULL;
    if (callable.t == T_CLOSURE) {
        clo = (TClosure *)callable.u.p;
        if (clo->bfn) bfn = (BuiltinFn)clo->bfn;
    } else if (callable.t == T_STR) {
        bool isb;
        void *hit = resolve_fn(vm, (TStr *)callable.u.p, &isb);
        if (!hit)
            vm_raise(vm, ex_class("Error"), NULL,
                     "Call to undefined function %s()",
                     tstr_val((TStr *)callable.u.p));
        if (isb) bfn = (BuiltinFn)hit; else uf = (TFunc *)hit;
    } else {
        vm_raise(vm, ex_class("TypeError"), NULL,
                 "Value is not callable");
    }
    if (bfn) {                       /* first-class builtin wrapper */
        return bfn(vm, args, n);
    }
    TFunc *fn = clo ? clo->fn : uf;
    if (vm->x.n_args + n > VM_MAX_ARGS)
        vm_raise(vm, ex_class("Error"), NULL, "Too many arguments");
    /* 排队：args 区持有自己的引用（与 SEND 主路径一致，addref）。
     * 非 variadic 只排 [0, n_params) 内的实参——超出部分留在调用方数组
     * 由调用方释放；variadic 全排，由 RECV_VARIADIC 搬走。
     * 这里不预搬参数进 slots：函数体首部的 RECV 负责从 args 区搬运，
     * 预搬 + 清空 args 区会让 RECV 读到 T_UNDEF 而误报
     * ArgumentCountError（M5-d on() 分发崩溃的根因）。 */
    uint32_t base = (uint32_t)vm->x.n_args;
    int nq = (fn->is_variadic || n < fn->n_params) ? n : fn->n_params;
    for (int i = 0; i < nq; i++) {
        vm->x.args[base + (uint32_t)i] = args[i];
        tval_addref(args[i]);
    }
    vm->x.n_args = (int)base + nq;
    VMCallRec *rec = call_push(vm, &g_magic_stub);
    rec->is_thunk = true;            /* RETURN stores into thunk_ret */
    rec->uf = fn;
    rec->args_base = base;
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = fn;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = base;
    f->n_args_sent = (uint16_t)nq;
    f->called_cls = NULL;
    f->n_calls_at_entry = vm->x.n_calls;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    if (clo)
        for (uint16_t i = 0; i < clo->n_cap; i++) {
            tval_addref(clo->captures[i]);
            f->slots[i] = clo->captures[i];
        }
    /* 形参/默认值/variadic 全部交给函数体首部的 RECV/RECV_VARIADIC（与
     * op_do_fcall 主路径一致），此处只恢复 args 区水位 */
    vm->x.n_args = (int)base;
    vm->x.frame = f;
    while (vm->x.frame == f) {
        TOp *cop = &f->fn->ops[f->pc];
        int32_t d = ((OpHandler)cop->handler)(vm, cop);
        if (d) f->pc += (uint32_t)d;
    }
    if (vm->x.exception)
        longjmp(vm->x.unwind, 1);
    return vm->x.thunk_ret;
}

/* 以闭包为 main 跑一个独立 runtime（M5-c：Worker 子线程入口）。
 * prog 为共享只读字节码；本线程的 thread-local runtime 必须已初始化。 */
int tphp_vm_run_closure(Program *prog, TClosure *clo) {
    VM *vm = &g_vm;
    memset(vm, 0, sizeof *vm);
    vm->prog = prog;
    vm_seed_consts(vm);
    tphp_vm_current = vm;
    tphp_raise_hook = vm_value_raise;
    tphp_tostr_hook = vm_tostring_hook;
    TFunc *fn = clo->fn;
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = fn;
    f->pc = 0;
    f->prev = NULL;
    f->args_base = 0;
    f->n_args_sent = 0;
    f->called_cls = NULL;
    f->n_calls_at_entry = 0;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    for (uint16_t i = 0; i < clo->n_cap; i++) {
        tval_addref(clo->captures[i]);
        f->slots[i] = clo->captures[i];
    }
    vm->x.frame = vm->main_frame = f;
    tphp_ctx_ensure(&vm->x);
    int rc = tphp_sched_run();
    tphp_raise_hook = NULL;
    tphp_vm_current = NULL;
    return rc;
}

int tphp_vm_run(Program *prog) {
    VM *vm = &g_vm;
    memset(vm, 0, sizeof *vm);
    vm->prog = prog;
    vm_seed_consts(vm);
    tphp_vm_current = vm;
    tphp_raise_hook = vm_value_raise;
    tphp_tostr_hook = vm_tostring_hook;
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = prog->main;
    f->pc = 0;
    f->prev = NULL;
    f->args_base = 0;
    f->n_args_sent = 0;
    f->called_cls = NULL;         /* main frame: global scope */
    f->n_calls_at_entry = 0;
    uint32_t nslots = (uint32_t)prog->main->n_locals + prog->main->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    vm->x.frame = vm->main_frame = f;
    tphp_ctx_ensure(&vm->x);
    /* M4: the scheduler takes over — the script becomes the main coroutine
     * and the event loop drives spawn'ed ones until all are done */
    int rc = tphp_sched_run();
    tphp_raise_hook = NULL;
    tphp_vm_current = NULL;
    return rc;
}

/* ---- opcode dump (--dump-opcodes) ---- */
static void dump_lit(TVal v, char *buf, size_t sz) {
    switch (v.t) {
    case T_NULL: snprintf(buf, sz, "null"); break;
    case T_FALSE: snprintf(buf, sz, "false"); break;
    case T_TRUE: snprintf(buf, sz, "true"); break;
    case T_INT: snprintf(buf, sz, "int(%lld)", (long long)v.u.i); break;
    case T_FLOAT: snprintf(buf, sz, "float(%.14G)", v.u.d); break;
    case T_STR: {
        TStr *s = (TStr *)v.u.p;
        snprintf(buf, sz, "string(%u) \"%.32s\"", s->len, tstr_val(s));
        break;
    }
    default: snprintf(buf, sz, "?"); break;
    }
}

static void dump_opnd(TOp *o, int w, TFunc *fn, char *buf, size_t sz) {
    uint8_t k = w == 1 ? o->t1 : w == 2 ? o->t2 : o->tr;
    uint32_t a = w == 1 ? o->a1 : w == 2 ? o->a2 : o->ar;
    switch (k) {
    case OPD_UNUSED: snprintf(buf, sz, "-"); break;
    case OPD_CONST: dump_lit(fn->lits[a], buf, sz); break;
    case OPD_CV:
        if (a < fn->n_locals && fn->local_names[a])
            snprintf(buf, sz, "$%s", tstr_val(fn->local_names[a]));
        else
            snprintf(buf, sz, "$cv%u", a);
        break;
    default: snprintf(buf, sz, "t%u", a); break;
    }
}

void tphp_dump_program(Program *p) {
    for (int fi = -1; fi < p->n_funcs; fi++) {
        TFunc *fn = fi < 0 ? p->main : p->funcs[fi];
        printf("%s %s():\n", fn->is_main ? "main" : "function",
               fn->name ? tstr_val(fn->name) : "{closure}");
        printf("  locals(%u):", fn->n_locals);
        for (uint16_t i = 0; i < fn->n_locals; i++)
            printf(" $%s", fn->local_names[i] ? tstr_val(fn->local_names[i]) : "?");
        printf("\n  tmps(%u)\n", fn->n_tmps);
        for (uint32_t pc = 0; pc < fn->n_ops; pc++) {
            TOp *o = &fn->ops[pc];
            char b1[96], b2[96], b3[96];
            printf("  %04u %-12s", pc, tphp_op_name(o->op));
            switch (o->op) {
            case OP_JMP: case OP_JMPZ: case OP_JMPNZ: case OP_JMP_NULL:
                dump_opnd(o, 1, fn, b1, sizeof b1);
                printf("%s -> %u\n", b1, o->ar);
                break;
            case OP_INIT_FCALL: case OP_INIT_DYN:
                dump_opnd(o, 1, fn, b1, sizeof b1);
                printf("%s args=%u\n", b1, o->a2);
                break;
            case OP_INC: case OP_DEC:
                dump_opnd(o, 1, fn, b1, sizeof b1);
                dump_opnd(o, 2, fn, b2, sizeof b2);
                dump_opnd(o, 3, fn, b3, sizeof b3);
                printf("%s, %s -> %s (%s)\n", b1, b2, b3,
                       o->ext ? "prefix" : "postfix");
                break;
            default:
                dump_opnd(o, 1, fn, b1, sizeof b1);
                dump_opnd(o, 2, fn, b2, sizeof b2);
                dump_opnd(o, 3, fn, b3, sizeof b3);
                printf("%s, %s -> %s\n", b1, b2, b3);
                break;
            }
        }
    }
}
