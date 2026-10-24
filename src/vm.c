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
#include "generator.h"
#include "bundle.h"
#include "pack.h"
#include "ast.h"
#include <uv.h>        /* uv_os_uname → PHP_UNAME */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

/* compile.c / parse.c（运行期 require 动态路径） */
Program *tphp_compile(AStmtList *ast, const char *file);
/* tphp_parse_script 在 ast.h */

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
static const TExClass EX_VALUEERR = { "ValueError", &EX_ERROR };   /* P1-2 */

extern const TExClass *tphp_ex_classes[];
extern const int tphp_ex_class_count;

const TExClass *tphp_ex_classes[] = {
    &EX_THROWABLE, &EX_ERROR, &EX_TYPE, &EX_ARITY, &EX_DIVZERO,
    &EX_UNDEF, &EX_COMPILE, &EX_EXCEPTION, &EX_RUNTIME, &EX_LOGIC,
    &EX_INVALIDARG, &EX_ERROREX, &EX_ASYNC_CANCEL, &EX_DEADLOCK,
    &EX_NOTCLONEABLE, &EX_UNMATCH, &EX_VALUEERR,
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

/* 值的类型名（错误措辞用；对拍 php-src "Value of type int is not callable"） */
static const char *val_type_name(TVal v) {
    switch (v.t) {
    case T_NULL: return "null";
    case T_FALSE: case T_TRUE: return "bool";
    case T_INT: return "int";
    case T_FLOAT: return "float";
    case T_STR: return "string";
    case T_ARR: return "array";
    default: return "object";
    }
}

/* ---- P2-B: 类常量惰性求值的环检测 ----
 * const A = self::B; const B = self::A; → 求值栈查重。异常 unwind 时
 * 复位（vm_unwind 入口调用），catch-and-retry 后可重新求值（对齐 PHP）。 */
static struct { TClass *cls; int idx; } g_const_eval[64];
static int g_n_const_eval;
static bool const_eval_in_progress(TClass *cls, int idx) {
    for (int i = 0; i < g_n_const_eval; i++)
        if (g_const_eval[i].cls == cls && g_const_eval[i].idx == idx)
            return true;
    return false;
}
static void const_eval_push(TClass *cls, int idx) {
    if (g_n_const_eval < 64) {
        g_const_eval[g_n_const_eval].cls = cls;
        g_const_eval[g_n_const_eval].idx = idx;
    }
    g_n_const_eval++;
}
static void const_eval_pop(void) { if (g_n_const_eval > 0) g_n_const_eval--; }
static void const_eval_reset(void) { g_n_const_eval = 0; }

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
    c->scope = NULL;         /* P1-3: 默认继承 fn->decl_cls */
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
                         const char *file, TExObj *previous) {
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
    x->file = file;           /* 抛出点文件（NULL = 未知，打印回落 prog） */
    x->wrap = NULL;      /* P1-2: 用户异常类包装；引擎异常为 NULL */
    return x;
}

/* ---- engine raise: store the exception and longjmp to the main loop ---- */
/* FFI 回调上下文中禁止 longjmp：unwind 会跳过 ffi_call 的 C 栈帧毁掉
 * libffi/C 库状态（PLAN/02 §4.5.1；PHP bailout 同病所以同样硬禁）。
 * 所有 vm->x.unwind 跳转必须经此（op_throw 直接 longjmp 曾绕过检查）。 */
static void vm_unwind_jump(VM *vm) __attribute__((noreturn));
static void vm_unwind_jump(VM *vm) {
    extern _Thread_local int tphp_ffi_cb_depth;
    if (tphp_ffi_cb_depth > 0) {
        const char *m = (vm->x.exception && vm->x.exception->message)
            ? tstr_val(vm->x.exception->message) : "exception";
        fprintf(stderr,
                "PHP Fatal error: Throwing from FFI callbacks is not allowed: %s\n",
                m);
        exit(255);
    }
    longjmp(vm->x.unwind, 1);
}

static void vm_raise_at(VM *vm, const TExClass *cls, int line,
                        const char *file, const char *fmt, va_list ap)
    __attribute__((noreturn));

static void vm_raise_at(VM *vm, const TExClass *cls, int line,
                        const char *file, const char *fmt, va_list ap) {
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    TStr *msg = tstr_new(buf, strlen(buf));
    vm->x.exception = tex_alloc(cls, msg, 0, line, file, NULL);
    vm_unwind_jump(vm);
}

/* 抛出点文件 = 当前帧的源文件（行号与文件同属一个 fn->file） */
static const char *raise_file(VM *vm) {
    return (vm->x.frame && vm->x.frame->file) ? vm->x.frame->file : NULL;
}

static void vm_raise(VM *vm, const TExClass *cls, TOp *op,
                     const char *fmt, ...)
    __attribute__((noreturn, format(printf, 4, 5)));

static void vm_raise(VM *vm, const TExClass *cls, TOp *op,
                     const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int line = op ? op->line : 0;
    if (!line && vm->x.n_calls > 0)
        line = vm->x.calls[vm->x.n_calls - 1].line;
    vm_raise_at(vm, cls, line, raise_file(vm), fmt, ap);
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
    vm_raise_at(vm, ex_class(cls), line, raise_file(vm), fmt, ap);
    va_end(ap);
}

/* value.c hook: convert engine value errors into exceptions */
static void vm_value_raise(const char *cls_name, const char *fmt, va_list ap)
    __attribute__((noreturn));

static void vm_value_raise(const char *cls_name, const char *fmt, va_list ap) {
    VM *vm = (VM *)tphp_vm_current;
    int line = 0;
    if (vm && vm->x.frame && vm->x.frame->pc < vm->x.frame->fn->n_ops)
        line = vm->x.frame->fn->ops[vm->x.frame->pc].line;
    if (!line && vm && vm->x.n_calls > 0)
        line = vm->x.calls[vm->x.n_calls - 1].line;
    vm_raise_at(vm, ex_class(cls_name), line, vm ? raise_file(vm) : NULL,
                fmt, ap);
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

/* main frame slot lookup by name (the main frame IS the global scope).
 * main_frame 在主帧 RETURN 时置 NULL（收尾析构期间 global 访问得到干净
 * 错误而非 UAF——2026-10-15 P0-2） */

/* B2: 动态全局条目（per-thread；见 global_slot 内注释） */
typedef struct { TStr *name; TVal val; } DygEnt;
static _Thread_local DygEnt *g_dyg;
static _Thread_local uint32_t g_ndyg, g_capdyg;

static TVal *global_slot(VM *vm, TStr *name) {
    if (vm->main_frame) {
        TFunc *fn = vm->main_frame->fn;
        for (uint16_t i = 0; i < fn->n_locals; i++)
            if (tstr_eq(fn->local_names[i], name))
                return &vm->main_frame->slots[i];
    }
    /* B2: 预扫描（scan_globals）只覆盖顶层语句嵌套与顶层函数体——方法/
     * 闭包内的 `global $x` 若 $x 顶层从未出现，main 帧没有槽。动态全局
     * 条目只会在这种场景创建（顶层出现该名 → main 槽必存在并先命中），
     * 故动态表与 main 帧槽永不分裂，语义即 PHP 全局符号表。 */
    for (uint32_t i = 0; i < g_ndyg; i++)
        if (tstr_eq(g_dyg[i].name, name)) return &g_dyg[i].val;
    return NULL;
}

/* B2: 创建动态全局条目（仅 op_assign_global 在 miss 时调用） */
static TVal *dyn_global_create(TStr *name) {
    if (g_ndyg == g_capdyg) {
        g_capdyg = g_capdyg ? g_capdyg * 2 : 16;
        g_dyg = tm_realloc(g_dyg, sizeof(DygEnt) * (size_t)g_capdyg);
    }
    g_dyg[g_ndyg].name = name;
    g_dyg[g_ndyg].val = (TVal){ .u.i = 0, .t = T_UNDEF };
    return &g_dyg[g_ndyg++].val;
}

/* 收尾释放（tphp_run_shutdown_dtors 末尾；幂等） */
static void dyn_global_release_all(void) {
    for (uint32_t i = 0; i < g_ndyg; i++) {
        tval_release(&g_dyg[i].val);
        g_dyg[i].val = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    g_ndyg = 0;
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
    (void)vm;
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
        g = dyn_global_create((TStr *)vm->x.frame->fn->lits[op->a2].u.p);  /* B2 */
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
    if (v.t == T_OBJ && ((GCHead *)v.u.p)->kind == GC_KIND_OBJ) {
        /* Generator（v1 对象 foreach 仅 Generator；一般 Iterator 后置） */
        if (tphp_generator_fe_reset(vm, v)) {
            tval_addref(v);
            wtake(vm, op, v);
            return 1;
        }
    }
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
    if (it.t == T_OBJ && ((GCHead *)it.u.p)->kind == GC_KIND_OBJ) {
        TVal key = TV_NULL, val = TV_NULL;
        bool more = false;
        if (tphp_generator_fe_fetch(vm, it, &key, &val, &more)) {
            if (!more) {
                wtake(vm, op, TV_FALSE);
                return 1;
            }
            TVal *vp = &vm->x.frame->slots[op->a2 + 1];
            tval_release(vp);
            *vp = val;                    /* 移交 +1 */
            TVal *kp = &vm->x.frame->slots[op->a2 + 2];
            tval_release(kp);
            *kp = key;
            wtake(vm, op, TV_TRUE);
            return 1;
        }
    }
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

/* ---- generators（PLAN/generators.md；handler → generator.c） ---- */

/* 生成器函数首条：帧已由 DO_FCALL 建立挂链、RECV 已搬参数入槽。
 * 打包 Generator 对象写回调用方 dst，摘帧返回控制（参数已安全落槽）。 */
static int32_t op_generator_create(VM *vm, TOp *op) {
    (void)op;
    VMFrame *f = vm->x.frame;
    TVal genobj = tphp_generator_pack(vm, f);
    VMCallRec *rec = (vm->x.n_calls > 0) ? &vm->x.calls[vm->x.n_calls - 1] : NULL;
    if (!rec || (rec->uf == NULL && rec->clo == NULL))
        tphp_vm_raise(vm, "Error", "Internal: generator create without call rec");
    VMFrame *caller = f->prev;
    if (rec->dst_kind != OPD_UNUSED && caller) {
        TVal *d = &caller->slots[rec->dst_slot];
        tval_release(d);
        *d = genobj;              /* 移交引用给调用方 dst */
    } else {
        tval_release(&genobj);
    }
    vm->x.n_calls--;
    f->pc++;                      /* 首驱动起点 = CREATE 之后 */
    f->prev = NULL;
    vm->x.frame = caller;
    return 0;                     /* caller->pc 已在 DO_FCALL 推进 */
}

/* 产出：暂停生成器帧、摘链交还驱动方 */
static int32_t op_yield(VM *vm, TOp *op) {
    VMFrame *f = vm->x.frame;
    if (!f->gen)
        tphp_vm_raise(vm, "Error", "Internal: yield outside a generator");
    TVal v = op->t1 != OPD_UNUSED ? rdv(vm, op, 1) : TV_NULL;
    TVal k = op->t2 != OPD_UNUSED ? rdv(vm, op, 2) : TV_NULL;
    bool has_key = op->t2 != OPD_UNUSED;
    uint32_t slot = (op->tr != OPD_UNUSED) ? op->ar : 0;
    bool has_send = op->tr != OPD_UNUSED;
    tphp_generator_pause(vm, f, v, k, has_key, slot, has_send);
    return 0;                     /* 帧已切换，勿再推进 */
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
    /* 运行期 require 注册的函数（inc_progs）——不写入共享 ic（未 freeze） */
    for (int pi = 0; pi < vm->n_inc_progs; pi++) {
        Program *ip = vm->inc_progs[pi];
        for (int i = 0; i < ip->n_funcs; i++)
            if (ip->funcs[i]->name &&
                !ip->funcs[i]->is_closure &&
                tstr_eq(ip->funcs[i]->name, name)) {
                *is_builtin = false;
                return ip->funcs[i];
            }
    }
    for (int i = 0; i < tphp_builtin_count; i++)
        if (strcmp(tphp_builtins[i].name, tstr_val(name)) == 0) {
            *is_builtin = true;
            return (void *)tphp_builtins[i].fn;
        }
    return NULL;
}

/* 返回 true = 来自 inc_progs（调用方勿把指针写进共享 bytecode ic） */
static bool resolve_fn_is_inc(VM *vm, TFunc *fn) {
    for (int pi = 0; pi < vm->n_inc_progs; pi++) {
        Program *ip = vm->inc_progs[pi];
        for (int i = 0; i < ip->n_funcs; i++)
            if (ip->funcs[i] == fn) return true;
    }
    return false;
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
        /* inc_progs 来源不写共享 ic（Program 未 freeze，跨线程不安全） */
        if (!resolve_fn_is_inc(vm, (TFunc *)hit))
            op->ic = hit;
    }
    rec->dst_kind = op->tr;
    rec->dst_slot = op->ar;
    return 1;
}

/* P2-B: dispatch a class+method pair from an array ['Cls','m']/[$obj,'m']
 * or a 'Cls::m' string callable; fills the call rec. o = receiver
 * (NULL = plain static call). Visibility is not re-checked (same as the
 * __invoke branch — known simplification). */
static void init_method_callable(VM *vm, TOp *op, VMCallRec *rec,
                                 TClass *cls, TObj *o, TStr *mname) {
    char mlc[128];
    { const char *s = tstr_val(mname);
      size_t k = 0; for (; s[k] && k < sizeof mlc - 1; k++)
          mlc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
      mlc[k] = 0; }
    tphp_class_ensure_flat(cls);
    TMethod *m = tphp_class_find_method(cls, mlc);
    if (!m || (!m->fn && !m->native) || m->is_abstract)
        vm_raise(vm, ex_class("Error"), op,
                 "Call to undefined method %s::%s()",
                 tstr_val(cls->name), tstr_val(mname));
    if (o && !m->is_static) {
        rec->is_method = true;
        rec->called_cls = cls;
        rec->this_obj = o;
        tval_addref(TV_OBJ(o));          /* the rec owns a receiver ref */
        if (!m->fn && m->native) {
            rec->is_builtin = true;
            rec->nm_fn = m->native;
        } else {
            rec->uf = m->fn;
            rec->scope_decl = m->decl;
        }
        return;
    }
    if (!m->is_static)
        vm_raise(vm, ex_class("Error"), op,
                 "Non-static method %s::%s() called statically",
                 tstr_val(cls->name), tstr_val(mname));
    if (!m->fn && m->native) {           /* static native (Worker::self 等) */
        rec->is_builtin = true;
        rec->nm_fn = m->native;
        rec->called_cls = cls;
    } else {
        rec->is_method = true;
        rec->called_cls = cls;
        rec->uf = m->fn;
        rec->scope_decl = m->decl;
    }
}

static int32_t op_init_dyn(VM *vm, TOp *op) {
    TVal fv = rdv(vm, op, 1);
    VMCallRec *rec = call_push(vm, op);
    rec->dst_kind = op->tr;
    rec->dst_slot = op->ar;
    if (fv.t == T_CLOSURE) {
        TClosure *clo = (TClosure *)fv.u.p;
        if (clo->fn == NULL && (clo->h.flags & GC_FLAG_MFCC)) {
            /* bound method closure (P2-A $obj->m(...)): dispatch as a
             * method call on the captured receiver. Visibility check is
             * not re-applied here (known simplification, see TODO). */
            TObj *o = (TObj *)clo->captures[0].u.p;
            TStr *mname = (TStr *)clo->captures[1].u.p;
            char mlc[128];
            { const char *s = tstr_val(mname);
              size_t k = 0; for (; s[k] && k < sizeof mlc - 1; k++)
                  mlc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
              mlc[k] = 0; }
            tphp_class_ensure_flat(o->cls);
            TMethod *m = tphp_class_find_method(o->cls, mlc);
            if (!m || !m->fn || m->is_abstract)
                vm_raise(vm, ex_class("Error"), op,
                         "Method %s::%s() is not callable",
                         tstr_val(o->cls->name), tstr_val(mname));
            rec->is_method = true;
            rec->this_obj = o;
            tval_addref(clo->captures[0]);   /* frame slot 0 owns a reference */
            rec->uf = m->fn;
            rec->scope_decl = m->decl;
            return 1;
        }
        rec->clo = clo;
        return 1;
    }
    if (fv.t == T_OBJ) {
        /* __invoke: an object used as `$obj(...)` */
        GCHead *h = (GCHead *)fv.u.p;
        if (h->kind == GC_KIND_OBJ) {
            TObj *o = (TObj *)h;
            tphp_class_ensure_flat(o->cls);
            TMethod *m = tphp_class_find_method(o->cls, "__invoke");
            if (m && !m->is_abstract && !m->is_static) {
                if (m->fn) {
                    rec->is_method = true;
                    rec->this_obj = o;
                    tval_addref(fv);      /* frame slot 0 owns a reference */
                    rec->uf = m->fn;
                    rec->scope_decl = m->decl;
                    return 1;
                }
                if (m->native) {
                    /* 内部类的 native __invoke（Async\Task 等）：
                     * 无字节码帧，走 builtin 分派 */
                    rec->is_method = true;
                    rec->is_builtin = true;
                    rec->this_obj = o;
                    tval_addref(fv);
                    rec->nm_fn = m->native;
                    rec->called_cls = o->cls;
                    return 1;
                }
            }
        }
        vm_raise(vm, ex_class("Error"), op, "Value is not callable");
    }
    if (fv.t == T_ARR) {
        /* P2-B: array callable [$obj,'m'] / ['Cls','m'] */
        TArr *a = (TArr *)fv.u.p;
        if (a->n != 2)
            vm_raise(vm, ex_class("Error"), op,
                     "Callable array must have exactly 2 elements");
        uint32_t i0 = 0;
        while (i0 < a->dused && a->data[i0].t == T_UNDEF) i0++;
        uint32_t i1 = i0 + 1;
        while (i1 < a->dused && a->data[i1].t == T_UNDEF) i1++;
        if (i0 >= a->dused || i1 >= a->dused)
            vm_raise(vm, ex_class("Error"), op,
                     "Callable array must have exactly 2 elements");
        TVal *e0 = &a->data[i0], *e1 = &a->data[i1];
        if (e1->t != T_STR)
            vm_raise(vm, ex_class("TypeError"), op,
                     "Method name must be a string");
        TClass *cls = NULL;
        TObj *o = NULL;
        if (e0->t == T_OBJ) {
            o = (TObj *)e0->u.p;
            cls = o->cls;
        } else if (e0->t == T_STR) {
            char clc[256];
            { const char *s = tstr_val((TStr *)e0->u.p);
              size_t k = 0; for (; s[k] && k < sizeof clc - 1; k++)
                  clc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
              clc[k] = 0; }
            extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
            cls = class_resolve_lc2(clc, tstr_val((TStr *)e0->u.p));
            if (!cls)
                vm_raise(vm, ex_class("Error"), op, "Class \"%s\" not found",
                         tstr_val((TStr *)e0->u.p));
        } else {
            vm_raise(vm, ex_class("TypeError"), op,
                     "Callable array element #1 must be an object or a class name");
        }
        init_method_callable(vm, op, rec, cls, o, (TStr *)e1->u.p);
        return 1;
    }
    if (fv.t != T_STR)
        vm_raise(vm, ex_class("Error"), op,
                 "Value of type %s is not callable", val_type_name(fv));
    TStr *name = (TStr *)fv.u.p;
    {
        /* P2-B: 'Cls::m' string callable — split on the LAST separator
         * (class names contain backslashes, never colons) */
        const char *s = tstr_val(name);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2)
            sep = p;
        if (sep) {
            size_t cl = (size_t)(sep - s);
            char clc[256];
            if (cl >= sizeof clc) cl = sizeof clc - 1;
            for (size_t i = 0; i < cl; i++)
                clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
            clc[cl] = 0;
            char mlc[128];
            { const char *s2 = sep + 2;
              size_t k = 0; for (; s2[k] && k < sizeof mlc - 1; k++)
                  mlc[k] = (s2[k] >= 'A' && s2[k] <= 'Z') ? (char)(s2[k] + 32) : s2[k];
              mlc[k] = 0; }
            extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
            TClass *cls = class_resolve_lc2(
                clc, tstr_val(tstr_intern(s, cl)));
            if (!cls)
                vm_raise(vm, ex_class("Error"), op, "Class \"%.*s\" not found",
                         (int)cl, s);
            init_method_callable(vm, op, rec, cls, NULL,
                                 tstr_intern(sep + 2, strlen(sep + 2)));
            return 1;
        }
    }
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
    /* raw read: the named-args default sentinel is a T_UNDEF literal and
     * must pass through (RECV turns it into the default value); only an
     * undefined CV variable raises */
    TVal v = rd(vm, op, 1);
    if (v.t == T_UNDEF && op->t1 == OPD_CV) {
        vm_raise(vm, ex_class("UndefinedError"), op, "Undefined variable $%s",
                 tstr_val(vm->x.frame->fn->local_names[op->a1]));
    }
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

/* B8: 参数不足错误补全 PHP 8 调用点尾巴——"%u passed in FILE on line N
 * and (exactly|at least) %u expected"。异常行仍是定义行（vm_raise 取
 * RECV op->line，对齐 PHP getLine()）；消息内行号取调用点 call rec。 */
static void raise_arg_count(VM *vm, VMFrame *f, TOp *op)
    __attribute__((noreturn));

static void raise_arg_count(VM *vm, VMFrame *f, TOp *op) {
    TFunc *fn = f->fn;
    uint32_t required = 0;
    for (uint16_t i = 0; i < fn->n_params; i++) {
        bool has_def = (fn->defaults && fn->defaults[i].t != T_UNDEF) ||
                       (fn->default_thunk && fn->default_thunk[i]);
        if (!has_def) required++;
    }
    const char *fname = fn->name ? tstr_val(fn->name) : "{closure}";
    if (vm->x.n_calls > 0) {
        int cline = vm->x.calls[vm->x.n_calls - 1].line;
        const char *file = (vm->prog && vm->prog->file) ? vm->prog->file : "-";
        vm_raise(vm, ex_class("ArgumentCountError"), op,
                 "Too few arguments to function %s(), %u passed in %s on line %d and %s %u expected",
                 fname, (unsigned)f->n_args_sent, file, cline,
                 required == fn->n_params ? "exactly" : "at least",
                 (unsigned)required);
    } else {
        vm_raise(vm, ex_class("ArgumentCountError"), op,
                 "Too few arguments to function %s()", fname);
    }
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
                raise_arg_count(vm, f, op);
            } else {
                v = default_arg_val(d);
            }
        } else {
            /* P1-1: 参数类型校验——先校验再 move（raise 时引用仍归 args 槽，
             * 不会泄漏）。默认值不在此校验（PHP 在声明期校验默认值）。 */
            if (f->fn->param_types && f->fn->param_types[idx]) {
                const char *given = NULL;
                if (!tphp_type_accepts(tstr_val(f->fn->param_types[idx]), v,
                                       (TClass *)f->called_cls, &given)) {
                    const char *pn = (idx < f->fn->n_locals && f->fn->local_names[idx])
                        ? tstr_val(f->fn->local_names[idx]) : "?";
                    vm_raise(vm, ex_class("TypeError"), op,
                             "%s(): Argument #%u ($%s) must be of type %s, %s given",
                             f->fn->name ? tstr_val(f->fn->name) : "{closure}",
                             idx + 1, pn,
                             tstr_val(f->fn->param_types[idx]),
                             given ? given : "?");
                }
            }
            /* move */
            vm->x.args[f->args_base + idx] = (TVal){ .u.i = 0, .t = T_UNDEF };
        }
    } else {
        TVal d = f->fn->defaults[idx];
        if (d.t == T_UNDEF && f->fn->default_thunk && f->fn->default_thunk[idx]) {
            v = tphp_call_thunk(vm, f->fn->default_thunk[idx]);
        } else if (d.t == T_UNDEF) {
            raise_arg_count(vm, f, op);
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
    /* P1-1: 变参元素类型校验（收集完再校验，raise 无半成品数组） */
    if (f->fn->param_types && fixed < f->fn->n_params &&
        f->fn->param_types[fixed]) {
        TStr *vt = f->fn->param_types[fixed];
        for (uint32_t i = 0; i < a->dused; i++) {
            if (a->data[i].t == T_UNDEF) continue;
            const char *given = NULL;
            if (!tphp_type_accepts(tstr_val(vt), a->data[i],
                                   (TClass *)f->called_cls, &given)) {
                TVal av = TV_ARR(a);
                tval_release(&av);
                vm_raise(vm, ex_class("TypeError"), op,
                         "%s(): Argument #%u ($%s) must be of type %s, %s given",
                         f->fn->name ? tstr_val(f->fn->name) : "{closure}",
                         fixed + 1,
                         (fixed < f->fn->n_locals && f->fn->local_names[fixed])
                             ? tstr_val(f->fn->local_names[fixed]) : "?",
                         tstr_val(vt), given ? given : "?");
            }
        }
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
        /* enum ::from / ::tryFrom / ::cases（方法名由 INIT_SMCALL 存入
         * rec->magic_name；kind 3 的 key 形如 "Cls::from"，取末段） */
        TClass *cls = (TClass *)rec->called_cls;
        const char *s = tstr_val(rec->magic_name);
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
                vm_raise(vm, ex_class("ValueError"), op,
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
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    /* 可见性 scope（PHP EG(scope)）优先级：
     *   1. rec->scope_decl = TMethod.decl——普通方法=声明类（B::f() 执行 A
     *      的方法体 scope=A，A 的 private 可见）；trait 拷贝=使用类（TU）
     *   2. fn->decl_cls ——兜底（无 TMethod 路径，方法形态门控）
     *   3. NULL → frame_scope_cls fallback called_cls——闭包 bindTo 重绑
     *      scope 走这里（与 called_cls 的 static:: 后绑定分离） */
    f->vis_scope = rec->scope_decl ? (TClass *)rec->scope_decl
                 : ((fn->is_method && fn->decl_cls)
                        ? (TClass *)fn->decl_cls : NULL);
    f->fn = fn;
    f->file = fn->file;
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
    /* P3: parent::__construct / self::__construct 帧的可见性作用域 =
     * 声明类（rec->called_cls）；static:: 仍走 called_cls（后绑定） */
    if (rec->scope_is_called && rec->called_cls)
        f->vis_scope = rec->called_cls;
    if (rec->is_method && rec->this_obj) {
        /* $this lives in slot 0 (after capture slots, which methods lack);
         * ownership of the reference moves from the call record */
        f->slots[0] = TV_OBJ((TObj *)rec->this_obj);
        rec->this_obj = NULL;
    }
    return 0;                     /* pc managed (new frame starts at 0) */
}

static int32_t op_return(VM *vm, TOp *op) {
    /* 生成器函数 RETURN：存 retval、关帧（不经 call rec——generator 帧的
     * DO_FCALL rec 已在 CREATE 时消费） */
    if (vm->x.frame->gen) {
        TVal r = TV_NULL;
        if (op->t1 != OPD_UNUSED) {
            r = rdv(vm, op, 1);
            tval_addref(r);
        }
        tphp_generator_finish(vm, r);   /* 移交 r 引用 */
        return 0;
    }
    TVal r = TV_NULL;
    if (op->t1 != OPD_UNUSED) {
        r = rdv(vm, op, 1);
        tval_addref(r);
    }
    /* P1-1: 返回类型校验（void/never/mixed 不校验；void 的返回值本就丢弃）。
     * 隐式 return（r = null）同样校验——PHP 对非空返回类型抛 TypeError。
     * B1: never 函数隐式返回 = 运行期可捕获 TypeError（对齐 PHP；带值
     * return 已在加载期拦截，走到这里的 OP_RETURN 只能是落到函数尾）。 */
    {
        TFunc *fn = vm->x.frame->fn;
        TStr *rt = fn->ret_type;
        if (rt && strcmp(tstr_val(rt), "never") == 0 && op->t1 == OPD_UNUSED) {
            vm_raise(vm, ex_class("TypeError"), op,
                     "%s(): never-returning function must not implicitly return",
                     fn->name ? tstr_val(fn->name) : "{closure}");
        }
        if (rt) {
            const char *ts = tstr_val(rt);
            bool skip = strcmp(ts, "void") == 0 || strcmp(ts, "never") == 0 ||
                        strcmp(ts, "mixed") == 0;
            if (!skip) {
                const char *given = NULL;
                if (!tphp_type_accepts(ts, r, (TClass *)vm->x.frame->called_cls,
                                       &given)) {
                    tval_release(&r);
                    vm_raise(vm, ex_class("TypeError"), op,
                             "%s(): Return value must be of type %s, %s returned",
                             fn->name ? tstr_val(fn->name) : "{closure}",
                             ts, given ? given : "?");
                }
            }
        }
    }
    if (vm->x.n_calls > 0) {
        VMCallRec *rec = &vm->x.calls[vm->x.n_calls - 1];
        vm->x.n_calls--;
        if (rec->is_ctor) {
            /* `new` always yields the object itself, never the ctor's
             * return value (PHP semantics) */
            tval_release(&r);
            /* $this 的引用是"移动"到 dst 的（op_new_user 的 rc++ 已移交给
             * ctor 帧槽）：不能再 addref，否则每个对象恒定多持一个引用，
             * rc 永远归不了零——__destruct 永不触发、对象全部泄漏
             * （2026-10-15 P0-2 审计修复）。帧销毁时释放槽内的副本。 */
            TVal objv = vm->x.frame->slots[0];   /* $this */
            TVal *d = &vm->x.frame->prev->slots[rec->dst_slot];
            tval_release(d);
            *d = objv;
        } else if (rec->is_thunk) {
            vm->x.thunk_ret = r;    /* ownership moves to the thunk caller */
        } else if (rec->dst_kind != OPD_UNUSED) {
            TVal *d = &vm->x.frame->prev->slots[rec->dst_slot];
            tval_release(d);
            /* include：无显式 return 时 PHP 返回 1 */
            if (rec->is_include && r.t == T_NULL)
                r = TV_INT(1);
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
    if (f == vm->main_frame) vm->main_frame = NULL;   /* 防 global_slot UAF */
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
    /* 经当前帧 fn 的归属 Program 取 funcs 表——动态 require 的文件是独立
     * Program，vm->prog 仍是外层程序（fn_idx 会越界 → 0xC0000005） */
    Program *prog = (Program *)vm->x.frame->fn->prog;
    if (!prog) prog = vm->prog;
    if (fn_idx >= (uint32_t)prog->n_funcs)
        vm_raise(vm, ex_class("Error"), op, "Internal: closure function missing");
    TFunc *fn = prog->funcs[fn_idx];
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
    /* ext=1: no INIT_NEW rec (UnhandledMatchError path) — zero args, no rec
     * to pop. Otherwise the INIT_NEW rec at n_calls-1 owns the args. */
    uint32_t base;
    uint16_t nargs;
    VMCallRec *rec = NULL;
    if (op->ext) {
        base = (uint32_t)vm->x.n_args;
        nargs = 0;
    } else {
        rec = &vm->x.calls[vm->x.n_calls - 1];
        base = rec->args_base;
        nargs = (uint16_t)(vm->x.n_args - (int)base);
    }
    TVal *args = vm->x.args + base;
    TStr *msg = tstr_new("", 0);
    int64_t code = 0;
    if (nargs > 0) {
        if (args[0].t != T_STR)
            vm_raise(vm, ex_class("TypeError"), op,
                     "Exception message must be a string");
        msg = (TStr *)args[0].u.p;
        tval_addref(TV_STR(msg));
    }
    if (nargs > 1 && args[1].t == T_INT) code = args[1].u.i;
    TExObj *prev = NULL;
    if (nargs > 2 && args[2].t == T_OBJ) {
        prev = (TExObj *)args[2].u.p;
        prev->h.rc++;                       /* exception owns the chain */
    }
    /* consume the queued ctor args (msg/prev took their own references):
     * release the area slots and pop them off, or the next call would
     * inherit them as phantom arguments */
    for (uint16_t i = 0; i < nargs; i++)
        if (args[i].t != T_UNDEF)
            tval_release(&args[i]);
    vm->x.n_args = (int)base;
    if (rec) vm->x.n_calls--;              /* pop INIT_NEW rec */
    wtake(vm, op, TV_OBJ(tex_alloc(cls, msg, code, op->line, raise_file(vm),
                                   prev)));
    return 1;
}

/* ---- P1-3: Closure 内部类 ----
 * $this 捕获（compile.c）落地后，call/bindTo 重绑定 "this" 捕获槽是真
 * 语义（闭包 TFunc 的 local_names[0..n_cap) 就是捕获名）。native 的
 * self 形参传 TClosure*（GC_KIND_CLO，引用计数同 GCHead）。 */
static _Thread_local TOp g_magic_stub;   /* 前置：定义在文件尾段 */
TVal tphp_vm_call_callable(struct VM *vm, TVal callable, TVal *args, int n);
static TClass *g_closure_cls;

static int16_t clo_this_capture(TClosure *c) {
    if (!c->fn) return -1;                     /* builtin 包装无捕获名 */
    for (uint16_t i = 0; i < c->n_cap && i < c->fn->n_locals; i++)
        if (c->fn->local_names[i] &&
            lc_eq(tstr_val(c->fn->local_names[i]), "this"))
            return (int16_t)i;
    return -1;
}

/* 重绑定 $this：闭包编译时带 "this" 捕获槽才可重绑；否则原样返回
 * （闭包体本就引用不到 $this，绑定无意义）。scope = 新作用域类
 * （PHP bindTo/bind 的第 2/3 参），决定 private/protected 可见性。 */
static TClosure *clo_rebind(TClosure *src, TVal newthis, TClass *scope) {
    if (src->bfn) return src;
    int16_t ti = clo_this_capture(src);
    if (ti < 0 && !scope) return src;
    TClosure *c = tclo_alloc(src->fn, src->captures, src->n_cap);
    if (ti >= 0) {
        tval_addref(newthis);
        tval_release(&c->captures[ti]);
        c->captures[ti] = newthis;
    }
    c->scope = scope;
    return c;
}

/* scope 参数解析：string 类名（或 ::class 字符串）→ TClass；null → NULL */
static TClass *clo_scope_arg(TVal v) {
    if (v.t != T_STR) return NULL;
    const char *s = tstr_val((TStr *)v.u.p);
    if (strncmp(s, "static::", 8) == 0) s += 8;   /* D::class 编译成字符串 */
    /* B3: 字面量 "static" = 保留闭包原 scope（PHP：传 "static" 不改变
     * 作用域，LSB 交给调用时 $this 的类回落解析） */
    if (strcmp(s, "static") == 0) return NULL;
    char lc[256];
    size_t k = 0;
    for (; s[k] && k + 1 < sizeof lc; k++)
        lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
    lc[k] = 0;
    extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
    return class_resolve_lc2(lc, s);
}

static TVal nm_closure_fromcallable(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n != 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Closure::fromCallable() expects exactly 1 argument");
    TVal cb = args[0];
    if (cb.t == T_CLOSURE) { tval_addref(cb); return cb; }
    if (cb.t == T_STR) {
        bool is_builtin = false;
        void *hit = resolve_fn_fb(vm, &g_magic_stub, (TStr *)cb.u.p, &is_builtin);
        if (hit) {
            TClosure *c = is_builtin ? tclo_alloc_builtin((BuiltinFn)hit)
                                     : tclo_alloc((TFunc *)hit, NULL, 0);
            return TV_CLO(c);
        }
    }
    tphp_vm_raise(vm, "TypeError",
                  "Closure::fromCallable(): Argument #1 ($callback) must be a valid callback");
    return TV_NULL;
}

static TVal nm_closure_bindto(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    TClosure *clo = (TClosure *)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Closure::bindTo() expects at least 1 argument");
    if (args[0].t != T_OBJ && args[0].t != T_NULL)
        tphp_vm_raise(vm, "TypeError",
                      "Closure::bindTo(): Argument #1 ($newThis) must be of type ?object");
    TClass *scope = n >= 2 ? clo_scope_arg(args[1]) : NULL;
    TClosure *out = args[0].t == T_OBJ || scope
        ? clo_rebind(clo, args[0], scope) : clo;
    TVal ov = TV_CLO(out);
    tval_addref(ov);
    return ov;
}

static TVal nm_closure_call(VM *vm, void *self, TVal *args, int n) {
    TClosure *clo = (TClosure *)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Closure::call() expects at least 1 argument");
    if (args[0].t != T_OBJ && args[0].t != T_NULL)
        tphp_vm_raise(vm, "TypeError",
                      "Closure::call(): Argument #1 ($newThis) must be of type ?object");
    TClosure *target = clo;
    if (args[0].t == T_OBJ) {
        /* PHP 语义：call() 以 $newThis 自身类作为临时作用域（private 可达） */
        TClass *scope = ((TObj *)args[0].u.p)->cls;
        target = clo_rebind(clo, args[0], scope);
    }
    TVal cv = TV_CLO(target);
    tval_addref(cv);
    TVal r = tphp_vm_call_callable(vm, cv, &args[1], n - 1);
    tval_release(&cv);
    return r;
}

static TVal nm_closure_bind(VM *vm, void *self, TVal *args, int n) {
    (void)self; (void)vm;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Closure::bind() expects at least 1 argument");
    if (args[0].t != T_CLOSURE)
        tphp_vm_raise(vm, "TypeError",
                      "Closure::bind(): Argument #1 ($closure) must be of type Closure");
    if (n >= 2 && args[1].t != T_OBJ && args[1].t != T_NULL)
        tphp_vm_raise(vm, "TypeError",
                      "Closure::bind(): Argument #2 ($newThis) must be of type ?object");
    TClass *scope = n >= 3 ? clo_scope_arg(args[2]) : NULL;
    TClosure *out = (n >= 2 && (args[1].t == T_OBJ || scope))
        ? clo_rebind((TClosure *)args[0].u.p, args[1], scope)
        : (TClosure *)args[0].u.p;
    TVal ov = TV_CLO(out);
    tval_addref(ov);
    return ov;
}

static void clsc_add_method(TClass *cls, const char *name, void *fn, bool is_static) {
    TMethod *ms = tm_malloc(sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    if (cls->methods)
        memcpy(ms, cls->methods, sizeof(TMethod) * (size_t)cls->n_methods);
    TMethod *m = &ms[cls->n_methods++];
    memset(m, 0, sizeof *m);
    m->name = tstr_new(name, strlen(name));
    m->visibility = 0;
    m->is_static = is_static;
    m->native = fn;
    tm_free(cls->methods);
    cls->methods = ms;
}

/* 注册 Closure 内部类（编译前；幂等）。线程表注册——与 Async\Coroutine
 * 同模式，worker 各自初始化。 */
static _Thread_local bool g_closure_inited;
void tphp_closure_class_init(void) {
    if (g_closure_inited) return;
    g_closure_inited = true;
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern("Closure", 7);
    cls->kind = CK_CLASS;
    cls->flattened = true;
    cls->m_ctor = cls->m_dtor = cls->m_get = cls->m_set = cls->m_isset =
        cls->m_unset = cls->m_call = cls->m_callstatic = cls->m_tostring =
        cls->m_invoke = cls->m_clone = cls->m_debuginfo = cls->m_clonestate = -1;
    clsc_add_method(cls, "fromCallable", (void *)nm_closure_fromcallable, true);
    clsc_add_method(cls, "bind", (void *)nm_closure_bind, true);
    clsc_add_method(cls, "call", (void *)nm_closure_call, false);
    clsc_add_method(cls, "bindTo", (void *)nm_closure_bindto, false);
    tphp_class_register(cls);
    g_closure_cls = cls;
}

static int32_t op_throw(VM *vm, TOp *op) {
    TVal v = rdv(vm, op, 1);
    if (v.t == T_OBJ && ((GCHead *)v.u.p)->kind == GC_KIND_OBJ) {
        /* P1-2: 用户异常类实例（isa Throwable）→ 包装成 TExObj 走统一异常
         * 机制。wrap 字段指向用户对象；catch 绑定/读消息都解包回它，
         * 用户类自带的方法与属性因此全部可见。 */
        TObj *o = (TObj *)v.u.p;
        if (!tphp_class_isa_name(o->cls, "throwable"))
            vm_raise(vm, ex_class("TypeError"), op,
                     "Can only throw objects");
        /* wrapper 的 TExClass = 用户类链上最近的内置异常祖先 */
        const TExClass *ecls = NULL;
        for (TClass *c = o->cls; c; c = c->parent) {
            int ci = tphp_ex_class_index(tstr_val(c->name));
            if (ci >= 0) { ecls = tphp_ex_classes[ci]; break; }
        }
        if (!ecls) ecls = ex_class("Error");
        TStr *msg = tstr_new("", 0);
        int64_t code = 0;
        int sm = tphp_class_find_prop(o->cls, "message");
        if (sm >= 0 && o->props[sm].t == T_STR) msg = (TStr *)o->props[sm].u.p;
        int sc = tphp_class_find_prop(o->cls, "code");
        if (sc >= 0 && o->props[sc].t == T_INT) code = o->props[sc].u.i;
        TExObj *w = tex_alloc(ecls, msg, code, op->line, raise_file(vm), NULL);
        tval_addref(TV_OBJ(o));
        w->wrap = o;                  /* 包装器持一份用户对象引用 */
        vm->x.exception = w;
        vm_unwind_jump(vm);
    }
    if (v.t != T_OBJ || ((GCHead *)v.u.p)->kind != GC_KIND_EX)
        vm_raise(vm, ex_class("TypeError"), op,
                 "Can only throw objects");
    TExObj *x = (TExObj *)v.u.p;
    x->line = op->line;
    x->file = raise_file(vm);         /* 抛出点文件（同 line 归因） */
    tval_addref(v);
    vm->x.exception = x;
    vm_unwind_jump(vm);
}

static int32_t op_bind_ex(VM *vm, TOp *op) {
    TExObj *x = vm->x.exception;
    vm->x.exception = NULL;         /* ownership moves into the catch slot */
    TVal *d = &vm->x.frame->slots[op->ar];
    tval_release(d);
    if (x->wrap) {
        /* P1-2: 绑定用户异常对象；包装器仅作传递，用完即释 */
        TVal out = TV_OBJ(x->wrap);
        tval_addref(out);
        d[0] = out;
        TVal wv = TV_OBJ(x);
        tval_release(&wv);
    } else {
        d[0] = TV_OBJ(x);
    }
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
/* 可见性作用域：parent::__construct 帧用声明类（vis_scope 覆盖位），
 * 其余帧沿用 called_cls（后绑定语义） */
static TClass *frame_scope_cls(VM *vm) {
    if (!vm->x.frame) return NULL;
    VMFrame *f = vm->x.frame;
    if (f->vis_scope) return (TClass *)f->vis_scope;
    return (TClass *)f->called_cls;
}

/* PHP 8.4 asymmetric visibility 写入作用域（对拍）：
 * private(set)：仅声明类本身；protected(set)：声明类及其子类。
 * （不复用 tphp_vis_ok——其 private 放行整个子类链，与 private(set) 语义不同） */
static bool asym_set_ok(uint8_t set_vis, TClass *owner, TClass *from) {
    if (set_vis == CVIS_PRIVATE) return from == owner;
    if (set_vis == CVIS_PROTECTED) {
        for (TClass *c = from; c; c = c->parent)
            if (c == owner) return true;
        return false;
    }
    return true;
}

/* "global scope" / "scope Sub"（写进 asymmetric 措辞；buf 供后者暂存） */
static const char *asym_scope(VM *vm, char *buf, size_t cap) {
    TClass *from = frame_scope_cls(vm);
    if (!from) return "global scope";
    snprintf(buf, cap, "scope %s", tstr_val(from->name));
    return buf;
}

/* B3: 方法可见性错误的 scope 措辞（PHP 对拍）——类作用域 → "scope X"，
 * 未带 scope 参数的闭包帧 → "scope Closure"，函数/全局 → "global scope" */
static const char *meth_scope_disp(VM *vm, char *buf, size_t cap) {
    TClass *from = frame_scope_cls(vm);
    if (from) { snprintf(buf, cap, "scope %s", tstr_val(from->name)); return buf; }
    if (vm->x.frame && vm->x.frame->fn && vm->x.frame->fn->is_closure)
        return "scope Closure";
    return "global scope";
}

/* exception getters callable as methods (M2 sugar): getMessage() etc */

/* C1: 成员名操作数读取——kind==CONST → 编译期字面量；否则取槽位值转字符串
 * （tval_to_str：int/float/bool/null 直转；数组/无 __toString 对象按项目
 * “诊断即异常”规则抛 TypeError）。转换结果存 thread-local scratch，
 * handler 取到后立即使用。 */
static _Thread_local TStr *g_mname_scratch;
static TStr *op_member_name(VM *vm, TOp *op, int which) {
    uint8_t kind;
    uint32_t a;
    if (which == 1) { kind = op->t1; a = op->a1; }
    else if (which == 2) { kind = op->t2; a = op->a2; }
    else { kind = op->tr; a = op->ar; }
    if (kind == OPD_CONST)
        return (TStr *)vm->x.frame->fn->lits[a].u.p;
    if (kind == OPD_UNUSED)
        return tstr_intern("", 0);
    TVal v = vm->x.frame->slots[a];
    if (v.t == T_STR) return (TStr *)v.u.p;      /* borrowed（槽持有引用） */
    if (v.t == T_UNDEF) return tstr_intern("", 0);
    TStr *s = tval_to_str(v);
    if (g_mname_scratch && g_mname_scratch != s) {
        TStr *old = g_mname_scratch;
        g_mname_scratch = NULL;
        tval_release(&(TVal){ .u.p = old, .t = T_STR });
    }
    g_mname_scratch = s;
    return s;
}

/* C1: 动态静态属性 key 拼接（与静态路径同构："Cls::$name" / "$name"；
 * 运行期名 intern 进全局串表，同名复用指针稳定）。 */
static TStr *dyn_sprop_key(int kind, const char *prefix, TStr *name) {
    static _Thread_local char kbuf[384];
    if (kind == 3)
        snprintf(kbuf, sizeof kbuf, "%s::$%s", prefix, tstr_val(name));
    else
        snprintf(kbuf, sizeof kbuf, "$%s", tstr_val(name));
    return tstr_intern(kbuf, strlen(kbuf));
}

/* C1: OSPROP 类成员名操作数——CONST → 原字面量（带 '$'）；
 * 动态 → 运行期值统一补 '$' 前缀（与静态形式同构）。 */
static TStr *op_member_name_os(VM *vm, TOp *op) {
    if (op->t2 == OPD_CONST)
        return (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    return dyn_sprop_key(0, "", op_member_name(vm, op, 2));
}

static bool ex_getter(const char *n, TExObj *x, TVal *out) {
    if (lc_eq(n, "getMessage"))   { *out = TV_STR(x->message); tval_addref(*out); return true; }
    if (lc_eq(n, "getCode"))      { *out = TV_INT(x->code); return true; }
    if (lc_eq(n, "getPrevious"))  { *out = x->previous ? TV_OBJ(x->previous) : TV_NULL;
                                    if (x->previous) tval_addref(*out); return true; }
    if (lc_eq(n, "getLine"))      { *out = TV_INT(x->line); return true; }
    return false;
}

/* ---- nested frame dispatcher (deep) ----
 * 跑到 f 真正弹出，子帧（动态 require 等）继续在本循环内执行。
 * 仅供 autoload 回调：回调体内 require 类文件时，"帧切换即退出"的旧式
 * 嵌套循环会提前收工造成帧栈错乱（2026-10-20 pkg autoload）。
 * 其余嵌套调用点保持各自的旧式 while (vm->x.frame == f)——历史行为
 * 依赖该提前退出，改深会破坏析构/magic 执行时序（实测大面积 0xC0000005）。 */
static void run_frame_until_pop_deep(VM *vm, VMFrame *f) {
    /* 必须先缓存 stop：f 弹出时已被 tm_free（op_return/frame_destroy），
     * 循环条件再读 f->prev 就是 UAF——曾导致 compare/autoload 场景错乱 */
    VMFrame *stop = f->prev;
    while (vm->x.frame && vm->x.frame != stop) {
        VMFrame *cf = vm->x.frame;
        TOp *cop = &cf->fn->ops[cf->pc];
        int32_t d = ((OpHandler)cop->handler)(vm, cop);
        if (d) cf->pc += (uint32_t)d;
    }
}

static void frame_destroy(VMFrame *f);

/* generator.c 桥接（同 tffi 模式） */
void tphp_vm_frame_destroy(VMFrame *f) { frame_destroy(f); }
void tphp_vm_run_nested_deep(VM *vm, VMFrame *f) { run_frame_until_pop_deep(vm, f); }

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
    TExObj *pre_ex = vm->x.exception;   /* 调用前已在飞行中的异常（见下方判别） */
    uint32_t base = (uint32_t)vm->x.n_args;
    for (int i = 0; i < n; i++)
        vm->x.args[base + (uint32_t)i] = args[i];
    vm->x.n_args = (int)base + n;
    VMCallRec *rec = call_push(vm, op);
    rec->is_method = true;
    rec->is_thunk = true;         /* RETURN stores into vm->x.thunk_ret */
    rec->uf = m->fn;
    rec->scope_decl = m->decl;
    TFunc *fn = m->fn;
    VMFrame *f = tm_malloc(sizeof *f);
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->vis_scope = rec->scope_decl ? (TClass *)rec->scope_decl
                 : ((fn->is_method && fn->decl_cls)
                        ? (TClass *)fn->decl_cls : NULL);
    f->fn = fn;
    f->file = fn->file;
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
    /* 只把"本次调用新抛出"的异常向上传播：调用前已在飞行中的异常
     * （析构器在 vm_unwind 途中被触发时会带着旧异常进来）原样保留，
     * 否则会被误判成魔术方法自己抛的——longjmp 打乱外层展开
     * （2026-10-15 P0-2：ctor 抛异常 + 类带 __destruct 的交互）。 */
    if (vm->x.exception && vm->x.exception != pre_ex) {
        /* __get etc raised: drop the queued args and propagate through
         * the normal unwind path */
        for (int i = 0; i < n; i++)
            tval_release(&vm->x.args[base + (uint32_t)i]);
        vm_unwind_jump(vm);
    }
    *out = vm->x.thunk_ret;
    return true;
}

/* ---- PHP 8.4 property hook invocation ----
 * 直接调用已编译的钩子 TFunc（方法形态：$this=slot0）。args 为实参（MOVED）。
 * 执行期间设置 (hook_obj, hook_prop) 上下文：钩子体内 $this->prop 直读/直写
 * backing，绕过钩子本身。返回前恢复外层上下文。 */
static bool call_hook(VM *vm, TOp *op, TObj *o, TStr *pname, TFunc *fn,
                      TVal *args, int nargs, TVal *out) {
    if (!fn) return false;
    if (vm->x.n_args + nargs > VM_MAX_ARGS)
        vm_raise(vm, ex_class("Error"), op, "Too many arguments");
    TExObj *pre_ex = vm->x.exception;
    uint32_t base = (uint32_t)vm->x.n_args;
    for (int i = 0; i < nargs; i++)
        vm->x.args[base + (uint32_t)i] = args[i];
    vm->x.n_args = (int)base + nargs;
    VMCallRec *rec = call_push(vm, op);
    rec->is_method = true;
    rec->is_thunk = true;
    rec->uf = fn;
    rec->called_cls = o->cls;
    VMFrame *f = tm_malloc(sizeof *f);
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->fn = fn;
    f->file = fn->file;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = base;
    f->n_args_sent = (uint16_t)nargs;
    f->called_cls = o->cls;
    f->vis_scope = fn->decl_cls ? (TClass *)fn->decl_cls : o->cls;
    f->n_calls_at_entry = vm->x.n_calls;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    f->slots[0] = TV_OBJ(o);     /* $this（方法形态 slot 0；无捕获） */
    tval_addref(f->slots[0]);

    void *saved_obj = vm->hook_obj;
    TStr *saved_prop = vm->hook_prop;
    vm->hook_obj = o;
    vm->hook_prop = pname;

    vm->x.frame = f;
    while (vm->x.frame == f) {
        TOp *cop = &f->fn->ops[f->pc];
        int32_t d = ((OpHandler)cop->handler)(vm, cop);
        if (d) f->pc += (uint32_t)d;
    }
    vm->x.n_args = (int)base;
    vm->hook_obj = saved_obj;
    vm->hook_prop = saved_prop;

    if (vm->x.exception && vm->x.exception != pre_ex) {
        for (int i = 0; i < nargs; i++)
            tval_release(&vm->x.args[base + (uint32_t)i]);
        vm_unwind_jump(vm);
    }
    *out = vm->x.thunk_ret;
    return true;
}

/* 钩子上下文判定：当前 (obj, prop) 正在执行其钩子 → $this->prop 走 backing */
static bool in_hook_backing(VM *vm, TObj *o, TStr *pname) {
    return vm->hook_obj == (void *)o && vm->hook_prop == pname;
}

/* OP_PROPHOOK：parent::$p::get()/set() — 从当前方法帧的 decl_cls->parent
 * 找属性钩子并调用。set 实参已在 args 栈顶（SEND 序列）。 */
static int32_t op_prophook(VM *vm, TOp *op) {
    TStr *pname = (TStr *)vm->x.frame->fn->lits[op->a1].u.p;
    /* prop 名含 '$' 前缀 → 去掉 */
    const char *raw = tstr_val(pname);
    const char *bare = (raw[0] == '$') ? raw + 1 : raw;
    TClass *decl = (TClass *)vm->x.frame->fn->decl_cls;
    if (!decl || !decl->parent)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot use parent::$%s::%s() outside a class hierarchy",
                 bare, op->ext ? "set" : "get");
    TClass *par = decl->parent;
    tphp_class_ensure_flat(par);
    int slot = tphp_class_find_prop(par, bare);
    if (slot < 0)
        vm_raise(vm, ex_class("Error"), op,
                 "Parent class %s has no property $%s",
                 tstr_val(par->name), bare);
    TProp *pp = &par->props[slot];
    /* this 对象：当前帧若是方法/钩子帧，slot 0 是 $this */
    TVal thisv = vm->x.frame->slots[0];
    if (thisv.t != T_OBJ)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot call parent::$%s::%s() without an object",
                 bare, op->ext ? "set" : "get");
    TObj *o = (TObj *)thisv.u.p;

    if (op->ext) {
        /* set：实参已在 args 栈顶（1 个） */
        TVal arg = TV_NULL;
        int nargs = 0;
        /* 从 SEND 序列看：SEND_VAL 推入 args；这里 n_args 已含它们。
         * 我们没有 INIT rec，故取栈顶 1 个作为 $value。 */
        if (vm->x.n_args > 0) {
            arg = vm->x.args[vm->x.n_args - 1];
            nargs = 1;
        }
        TVal out;
        if (!pp->hook_set)
            vm_raise(vm, ex_class("Error"), op,
                     "Parent property %s::$%s has no set hook",
                     tstr_val(par->name), bare);
        bool ok = call_hook(vm, op, o, pp->name, pp->hook_set, &arg, nargs, &out);
        /* 释放 SEND 进来的引用 */
        if (nargs)
            tval_release(&vm->x.args[vm->x.n_args - 1]);
        vm->x.n_args -= nargs;
        if (ok) tval_release(&out);
        if (op->tr != OPD_UNUSED) {
            TVal *d = &vm->x.frame->slots[op->ar];
            tval_release(d);
            d[0] = TV_NULL;   /* set 返回 void */
        }
        return 1;
    }
    /* get */
    TVal out;
    if (!pp->hook_get) {
        /* parent 无 get 钩子 → 读 backing（若可见） */
        if (!tphp_vis_ok(pp->visibility, par, frame_scope_cls(vm)))
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot access %s property %s::$%s",
                     vis_name(pp->visibility), tstr_val(par->name), bare);
        TVal v = o->props[slot];
        tval_addref(v);
        wtake(vm, op, v);
        return 1;
    }
    if (!call_hook(vm, op, o, pp->name, pp->hook_get, NULL, 0, &out))
        vm_raise(vm, ex_class("Error"), op,
                 "Parent property %s::$%s has no get hook",
                 tstr_val(par->name), bare);
    wtake(vm, op, out);
    return 1;
}

static int32_t op_init_mcall(VM *vm, TOp *op) {
    TVal objv = rdv(vm, op, 1);
    TStr *mname = op_member_name(vm, op, 3);   /* C1: CONST 字面量或动态名 */
    VMCallRec *rec = call_push(vm, op);
    /* P1-3: $closure->call()/bindTo() —— TClosure 接收者派发到内部
     * Closure 类（native；self 传 TClosure*）。 */
    if (objv.t == T_CLOSURE) {
        TMethod *m = g_closure_cls
            ? tphp_class_find_method(g_closure_cls, tstr_val(mname)) : NULL;
        if (!m || !m->native || m->is_static)
            vm_raise(vm, ex_class("Error"), op,
                     "Call to undefined method Closure::%s()",
                     tstr_val(mname));
        rec->is_builtin = true;
        rec->nm_fn = m->native;
        rec->is_method = true;
        rec->this_obj = objv.u.p;     /* TClosure*（GCHead 兼容引用计数） */
        tval_addref(objv);
        return 1;
    }
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
    /* B3: owner = 声明类（TMethod.decl；内部类无 decl 用查找类兜底）——
     * 关闭 M3 差异：父类 private 方法对子类 scope 不可见（对齐 PHP） */
    bool m_hidden = m && !tphp_vis_ok(m->visibility,
                                      m->decl ? m->decl : o->cls,
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
        if (m_hidden) {
            char sb[160];
            vm_raise(vm, ex_class("Error"), op,
                     "Call to %s method %s::%s() from %s",
                     vis_name(m->visibility),
                     tstr_val(m->decl ? m->decl->name : o->cls->name),
                     tstr_val(mname), meth_scope_disp(vm, sb, sizeof sb));
        }
        vm_raise(vm, ex_class("Error"), op,
                 "Call to undefined method %s::%s()",
                 tstr_val(o->cls->name), tstr_val(mname));
    }
    rec->is_method = true;
    rec->this_obj = o;
    tval_addref(objv);            /* frame slot 0 owns a reference */
    rec->uf = m->fn;
    rec->scope_decl = m->decl;
    return 1;
}

/* OP_INIT_NEW: snapshot args_base before ctor sends (spread-safe count;
 * same pattern as INIT_FCALL). The rec is consumed by NEW_USER/NEW_EX/
 * NEW_LATE/NEW_DYN — user-ctor path reuses it as the ctor rec. */
static int32_t op_init_new(VM *vm, TOp *op) {
    call_push(vm, op);
    return 1;
}

/* shared construction after the class is resolved. The INIT_NEW rec at
 * n_calls-1 holds args_base; real count = n_args - args_base (spread-safe). */
static int32_t new_user_at(VM *vm, TOp *op, TClass *cls, TStr *cname) {
    if (cls->flags & CLS_ABSTRACT)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot instantiate abstract class %s", tstr_val(cls->name));
    if (cls->kind == CK_INTERFACE || cls->kind == CK_TRAIT)
        vm_raise(vm, ex_class("Error"), op, "Cannot instantiate %s",
                 tstr_val(cls->name));
    if (cls == g_closure_cls)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot instantiate Closure");   /* P1-3 */
    /* P3: 构造器可见性——private/protected ctor 阻断外部实例化 */
    if (cls->m_ctor >= 0) {
        TMethod *cm = &cls->methods[cls->m_ctor];
        if (!tphp_vis_ok(cm->visibility, cm->decl ? cm->decl : cls,
                         frame_scope_cls(vm))) {
            TClass *from = frame_scope_cls(vm);
            vm_raise(vm, ex_class("Error"), op,
                     "Call to %s %s::__construct() from %s",
                     vis_name(cm->visibility), tstr_val(cls->name),
                     from ? tstr_val(from->name) : "global scope");
        }
    }
    VMCallRec *rec = &vm->x.calls[vm->x.n_calls - 1];  /* INIT_NEW */
    uint32_t base = rec->args_base;
    uint16_t nargs = (uint16_t)(vm->x.n_args - (int)base);
    rec->n_args = nargs;
    TVal *args = &vm->x.args[base];
    (void)cname;
    TObj *o = tphp_obj_alloc(cls);
    o->h.rc++;                    /* ctor invocation holds a reference */
    if (cls->m_ctor >= 0 && !cls->methods[cls->m_ctor].fn &&
        cls->methods[cls->m_ctor].native) {
        /* native constructor (M4 internal classes, e.g. Async\Channel):
         * run the C ctor with the sent args, then land the object */
        NativeMethodFn ctor = (NativeMethodFn)cls->methods[cls->m_ctor].native;
        TVal r = ctor(vm, o, args, (int)nargs);
        for (uint16_t i = 0; i < nargs; i++)
            if (args[i].t != T_UNDEF) tval_release(&args[i]);
        vm->x.n_args = (int)base;
        vm->x.n_calls--;                    /* pop INIT_NEW rec */
        tval_release(&r);
        o->h.rc--;                    /* 退掉 ctor-invocation 引用（native 路径） */
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
        /* reuse the INIT_NEW rec as the ctor rec (already on the stack) */
        rec->is_method = true;
        rec->is_ctor = true;
        rec->this_obj = NULL;      /* $this moves straight into the frame */
        rec->uf = cls->methods[cls->m_ctor].fn;
        rec->scope_decl = cls->methods[cls->m_ctor].decl;
        rec->dst_kind = op->tr;
        rec->dst_slot = op->ar;
        TFunc *fn = rec->uf;
        VMFrame *f = tm_malloc(sizeof *f);
        memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
        f->vis_scope = rec->scope_decl ? (TClass *)rec->scope_decl
                     : ((fn->is_method && fn->decl_cls)
                            ? (TClass *)fn->decl_cls : NULL);
        f->fn = fn;
        f->file = fn->file;
        f->pc = 0;
        f->prev = vm->x.frame;
        f->args_base = base;
        f->n_args_sent = nargs;
        f->n_calls_at_entry = vm->x.n_calls;
        f->gen = NULL;
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
        vm->x.n_args = (int)base;
        vm->x.frame->pc++;           /* caller resumes after NEW_USER */
        vm->x.frame = f;
        return 0;                  /* pc managed: ctor frame is running */
    }
    /* no constructor: release the sent args (INIT_NEW rec owns the base),
     * pop the rec, and land the object */
    for (uint16_t i = 0; i < nargs; i++)
        if (args[i].t != T_UNDEF) tval_release(&args[i]);
    vm->x.n_args = (int)base;
    vm->x.n_calls--;
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_release(d);
        o->h.rc--;                    /* 无构造器：退掉未发生的 ctor 引用 */
        d[0] = TV_OBJ(o);         /* transfer the +1 */
    } else {
        o->h.rc--;
        tval_release(&(TVal){ .u.p = o, .t = T_OBJ });
    }
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
    extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
    TClass *cls = class_resolve_lc2(lc, tstr_val(cname));
    if (!cls || cls->kind != CK_CLASS)
        vm_raise(vm, ex_class("Error"), op, "Class \"%s\" not found",
                 tstr_val(cname));
    return new_user_at(vm, op, cls, cname);
}

/* new self / new parent / new static — a1 = kind (0/1/2), same as smcall */
static TClass *smcall_cls(VM *vm, TOp *op, int kind, TStr *named);
static int32_t op_new_late(VM *vm, TOp *op) {
    TClass *cls = smcall_cls(vm, op, (int)op->a1, NULL);
    return new_user_at(vm, op, cls, cls->name);
}

/* new $cls / new ($expr) / new $a->b — o1 = class-name value.
 * PHP accepts a string (class name) or an object (uses its class). */
static int32_t op_new_dyn(VM *vm, TOp *op) {
    TVal cv = rdv(vm, op, 1);
    TStr *cname = NULL;
    if (cv.t == T_STR) {
        cname = (TStr *)cv.u.p;
    } else if (cv.t == T_OBJ) {
        GCHead *h = (GCHead *)cv.u.p;
        if (h->kind == GC_KIND_OBJ)
            cname = ((TObj *)h)->cls->name;
        else if (h->kind == GC_KIND_EX)
            cname = tstr_intern(((TExObj *)h)->cls->name,
                                strlen(((TExObj *)h)->cls->name));
        else
            vm_raise(vm, ex_class("Error"), op,
                     "Class name must be a valid object or a string");
    } else {
        vm_raise(vm, ex_class("Error"), op,
                 "Class name must be a valid object or a string");
    }
    char lc[256];
    { const char *s = tstr_val(cname);
      size_t k = 0; for (; s[k] && k < sizeof lc - 1; k++)
          lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
      lc[k] = 0; }
    extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
    TClass *cls = class_resolve_lc2(lc, tstr_val(cname));
    if (!cls)
        vm_raise(vm, ex_class("Error"), op, "Class \"%s\" not found",
                 tstr_val(cname));
    return new_user_at(vm, op, cls, cname);
}

/* B3: 实例属性可见性的 owner = 属性声明类（TProp.decl，与 asym 检查同源）
 * ——private/protected 按声明类判定，非接收者类（PHP 语义；bindTo scope
 * 对拍的关键：scope=声明类可读父类 private，scope=子类不可读） */
static TClass *prop_owner(TObj *o, int slot) {
    TProp *p = &o->cls->props[slot];
    return p->decl ? p->decl : o->cls;
}

static int32_t op_assign_prop(VM *vm, TOp *op) {
    TVal objv = rdv(vm, op, 1);
    TStr *name = op_member_name(vm, op, 2);   /* C1 */
    TVal val = rdv(vm, op, 3);    /* o3: operands are obj, name, value */
    if (objv.t != T_OBJ || ((GCHead *)objv.u.p)->kind != GC_KIND_OBJ)
        vm_raise(vm, ex_class("Error"), op,
                 "Attempt to assign property \"%s\" on %s", tstr_val(name),
                 val_type_name(objv));
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
    if (slot >= 0) {
        /* PHP 8.4 asymmetric visibility：显式 (set) 与读可见性不同时，
         * 写入按 set 可见性判定，失败直接 Error（不回退 __set） */
        TProp *ap = &o->cls->props[slot];
        if (ap->set_visibility && ap->set_visibility != ap->visibility) {
            TClass *owner = ap->decl ? ap->decl : o->cls;
            if (!asym_set_ok(ap->set_visibility, owner, frame_scope_cls(vm))) {
                char sb[160];
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot modify %s(set) property %s::$%s from %s",
                         vis_name(ap->set_visibility), tstr_val(owner->name),
                         tstr_val(name), asym_scope(vm, sb, sizeof sb));
            }
        }
    }
    if (slot >= 0 &&
        !tphp_vis_ok(o->cls->props[slot].visibility, prop_owner(o, slot),
                     frame_scope_cls(vm))) {
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
    /* ---- PHP 8.4 property hooks：写路径（可见性之后） ---- */
    if (slot >= 0) {
        TProp *ap = &o->cls->props[slot];
        if (ap->has_hooks && !in_hook_backing(vm, o, ap->name)) {
            if (ap->hook_set) {
                TVal out;
                TVal arg = val;
                tval_addref(arg);
                if (call_hook(vm, op, o, ap->name, ap->hook_set,
                              &arg, 1, &out)) {
                    tval_release(&out);   /* set 钩子返回值丢弃（void 语义） */
                }
                /* 赋值表达式返回 RHS（对齐 __set 行为） */
                if (op->tr != OPD_UNUSED) {
                    tval_addref(val);
                    TVal *d = &vm->x.frame->slots[op->ar];
                    tval_release(d);
                    d[0] = val;
                }
                return 1;
            }
            if (ap->is_virtual)
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot modify virtual property %s::$%s without set hook",
                         tstr_val(o->cls->name), tstr_val(name));
            /* 有 backing 无 set 钩子 → 落回默认写（下方类型/readonly 检查） */
        }
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
    /* P3: readonly 属性 / readonly class 写入规则——
     *  - 只允许"声明类"作用域内写入（子类也不行，PHP 语义）
     *  - 已初始化后再写：任何作用域都 fatal
     *  - readonly class 的全部实例属性按 readonly 处理 */
    {
        TProp *pp = &o->cls->props[slot];
        bool ro = pp->is_readonly || (o->cls->flags & CLS_READONLY);
        if (ro) {
            TClass *from = frame_scope_cls(vm);
            bool initialized = o->props[slot].t != T_UNDEF;
            if (from != pp->decl) {
                if (initialized)
                    vm_raise(vm, ex_class("Error"), op,
                             "Cannot modify readonly property %s::$%s from %s",
                             tstr_val(o->cls->name), tstr_val(name),
                             from ? tstr_val(from->name) : "global scope");
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot initialize readonly property %s::$%s from %s",
                         tstr_val(o->cls->name), tstr_val(name),
                         from ? tstr_val(from->name) : "global scope");
            }
            if (initialized)
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot modify readonly property %s::$%s",
                         tstr_val(o->cls->name), tstr_val(name));
        }
    }
    /* P1-1: typed 属性赋值校验 */
    if (o->cls->props[slot].type) {
        const char *given = NULL;
        if (!tphp_type_accepts(tstr_val(o->cls->props[slot].type), val,
                               o->cls, &given))
            vm_raise(vm, ex_class("TypeError"), op,
                     "Cannot assign %s to property %s::$%s of type %s",
                     given ? given : "?",
                     tstr_val(o->cls->name), tstr_val(name),
                     tstr_val(o->cls->props[slot].type));
    }
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
    TStr *name = op_member_name(vm, op, 2);   /* C1 */
    bool set = false;
    if (objv.t == T_OBJ) {
        GCHead *h = (GCHead *)objv.u.p;
        if (h->kind == GC_KIND_OBJ) {
            TObj *o = (TObj *)h;
            int slot = tphp_class_find_prop(o->cls, tstr_val(name));
            if (slot >= 0) {
                TProp *pp = &o->cls->props[slot];
                if (tphp_vis_ok(pp->visibility, prop_owner(o, slot),
                                frame_scope_cls(vm))) {
                    /* PHP 8.4：isset 带 get 钩子 → 调钩子，结果非 null 为 true */
                    if (pp->has_hooks && pp->hook_get &&
                        !in_hook_backing(vm, o, pp->name)) {
                        TVal out;
                        if (call_hook(vm, op, o, pp->name, pp->hook_get,
                                      NULL, 0, &out)) {
                            set = out.t != T_NULL;
                            tval_release(&out);
                        }
                    } else if (pp->has_hooks && pp->is_virtual && !pp->hook_get) {
                        vm_raise(vm, ex_class("Error"), op,
                                 "Cannot read virtual property %s::$%s without get hook",
                                 tstr_val(o->cls->name), tstr_val(name));
                    } else {
                        set = pp->is_virtual ? false : o->props[slot].t != T_UNDEF;
                    }
                }
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

/* B3: 当前帧的 $this 捕获槽对应的类（闭包 n_cap 区按名找 "this" 槽；
 * 方法帧 decl/called 必非空，走不到此回落） */
static TClass *frame_this_cls(VM *vm) {
    VMFrame *f = vm->x.frame;
    TFunc *fn = f->fn;
    for (uint16_t i = 0; i < fn->n_cap && i < fn->n_locals; i++)
        if (fn->local_names[i] && lc_eq(tstr_val(fn->local_names[i]), "this")) {
            TVal v = f->slots[i];
            if (v.t == T_OBJ) return ((TObj *)v.u.p)->cls;
            return NULL;
        }
    return NULL;
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
        if (!c) c = frame_this_cls(vm);   /* B3: 闭包 static:: 回落 $this 类 */
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
        extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
        TClass *c = class_resolve_lc2(lc, s);
        if (!c)
            vm_raise(vm, ex_class("Error"), op,
                     "Class \"%s\" not found", s);
        return c;
    }
    }
}

/* shared: called_cls resolution for a frame being created.
 * P1-3: 闭包若经 bindTo/bind 重绑过作用域（clo->scope），frame 的
 * called_cls 用它——private/protected 可见性与 self:: 随新作用域。 */
static TClass *frame_called_cls(VMCallRec *rec, TFunc *fn) {
    if (rec->is_method)
        return rec->this_obj ? ((TObj *)rec->this_obj)->cls
                             : (TClass *)rec->called_cls;
    if (rec->clo && rec->clo->scope)
        return rec->clo->scope;
    return (TClass *)fn->decl_cls;
}

static int32_t op_init_smcall(VM *vm, TOp *op) {
    /* C1: 动态名放 t1（argc 复用 a2，动态时 a1 被 name 占用 → kind 挂 ext）；
     * 静态保持 o1={UNUSED,kind}、a1=kind */
    bool dynm = op->t1 != OPD_UNUSED;
    int kind = dynm ? (int)op->ext : (int)op->a1;
    TStr *key = (TStr *)vm->x.frame->fn->lits[op->ar].u.p;
    TStr *dyn = dynm ? op_member_name(vm, op, 1) : NULL;
    TClass *cls;
    TStr *mname;
    if (kind == 3) {
        /* "Cls::method": class names may contain backslashes, never colons */
        const char *s = tstr_val(key);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2)
            sep = p;
        if (!sep && !dyn)
            vm_raise(vm, ex_class("Error"), op, "Malformed static call key");
        /* 动态时 key 是纯类名（无 "::method"），整段作类名 */
        size_t cl = sep ? (size_t)(sep - s) : strlen(s);
        char clc[128];
        if (cl >= sizeof clc) cl = sizeof clc - 1;
        for (size_t i = 0; i < cl; i++)
            clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
        clc[cl] = 0;
        extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
        cls = class_resolve_lc2(clc, tstr_val(tstr_intern(s, cl)));
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class \"%.*s\" not found",
                     (int)cl, s);
        mname = dyn ? dyn : tstr_intern(sep + 2, strlen(sep + 2));
    } else {
        cls = smcall_cls(vm, op, kind, NULL);
        mname = dyn ? dyn : key;
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
            rec->magic_name = mname;   /* P1-2: 方法名直接入 rec——DO_FCALL
                                         * 不再用 op-1-n_args 反查（带参调用
                                         * 时该公式指错，from() 非法值曾静默
                                         * 返回 null） */
            rec->called_cls = cls;
            return 1;
        }
    }
    if (!m || (!m->fn && !m->native) || m->is_abstract) {
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
         * current $this (caller slot 0); expression result is NULL.
         * P1-2: 内部异常类的原生 ctor（fn NULL + native）同样支持——走
         * nm_fn 带接收者分发（op_do_fcall native 路径）。
         * P3: 帧作用域取 called_cls（声明类）而非接收者类。 */
        rec->is_method = true;
        rec->called_cls = cls;
        rec->scope_is_called = true;
        if (!m->fn && m->native) {
            rec->is_builtin = true;
            rec->nm_fn = m->native;
        } else {
            rec->uf = m->fn;
            rec->scope_decl = m->decl;
        }
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
            /* B3: PHP 对该路径同样做方法可见性检查（caller scope vs 声明类）
             * ——parent::privateMethod() 从子类 scope 报错，__call 兜底 */
            if (!tphp_vis_ok(m->visibility, m->decl ? m->decl : cls,
                             frame_scope_cls(vm))) {
                TMethod *mc = tphp_class_find_method(cls, "__call");
                if (mc && mc->fn && !mc->is_abstract) {
                    TObj *mo = (TObj *)vm->x.frame->slots[0].u.p;
                    mo->h.rc++;
                    rec->magic_kind = 1;
                    rec->magic_name = mname;
                    rec->is_method = true;
                    rec->called_cls = cls;
                    rec->this_obj = mo;
                    return 1;
                }
                char sb[160];
                vm_raise(vm, ex_class("Error"), op,
                         "Call to %s method %s::%s() from %s",
                         vis_name(m->visibility),
                         tstr_val(m->decl ? m->decl->name : cls->name),
                         tstr_val(mname), meth_scope_disp(vm, sb, sizeof sb));
            }
            TObj *o = (TObj *)vm->x.frame->slots[0].u.p;
            o->h.rc++;               /* reference moves into the new frame */
            rec->is_method = true;
            rec->called_cls = cls;
            rec->this_obj = o;
            if (!m->fn && m->native) {   /* P1-2: 内部类原生实例方法 */
                rec->is_builtin = true;
                rec->nm_fn = m->native;
            } else {
                rec->uf = m->fn;
                rec->scope_decl = m->decl;
            }
            return 1;
        }
        vm_raise(vm, ex_class("Error"), op,
                 "Non-static method %s::%s() called statically",
                 tstr_val(cls->name), tstr_val(mname));
    }
    if (!tphp_vis_ok(m->visibility, m->decl ? m->decl : cls,
                     frame_scope_cls(vm))) {
        TMethod *mcs = tphp_class_find_method(cls, "__callstatic");
        if (mcs && mcs->fn && !mcs->is_abstract) {
            rec->magic_kind = 2;
            rec->magic_name = mname;
            rec->is_method = true;
            rec->called_cls = cls;
            return 1;
        }
        {
            char sb[160];
            vm_raise(vm, ex_class("Error"), op,
                     "Call to %s method %s::%s() from %s",
                     vis_name(m->visibility),
                     tstr_val(m->decl ? m->decl->name : cls->name),
                     tstr_val(mname), meth_scope_disp(vm, sb, sizeof sb));
        }
    }
    rec->is_method = true;
    rec->called_cls = cls;
    rec->uf = m->fn;
    rec->scope_decl = m->decl;
    return 1;
}

/* 静态属性存储路由（PHP 继承共享语义）：未重声明的继承条目 TProp.decl 指向
 * 声明类 → 读写声明类的运行时槽（A::$x 与子类实例视图共享）；重声明条目
 * decl=子类 → 自有槽。 */
static TVal *sprop_slot(TClass *cls, TProp *sp) {
    TClass *owner = sp->decl ? sp->decl : cls;
    return &tphp_class_sprop_vals(owner)[sp->slot];
}

static int32_t op_fetch_cconst(VM *vm, TOp *op) {
    int kind;
    TStr *key;
    if (op->t1 != OPD_UNUSED) {
        /* C1: C::$$v 动态——t1=成员名，ext=kind，t2=类名前缀(kind3)；
         * 运行期拼 key 后走与静态完全相同的解析 */
        kind = (int)op->ext;
        TStr *mn = op_member_name(vm, op, 1);
        TStr *pf = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
        key = dyn_sprop_key(kind, tstr_val(pf), mn);
    } else {
        kind = (int)op->a1;
        key = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    }
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
        extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
        cls = class_resolve_lc2(clc, tstr_val(tstr_intern(s, cl)));
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class \"%.*s\" not found",
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
        /* 可见性按属性声明类判（PHP：static:: LSB 只定解析不改 scope——
         * 8.5.1 对拍 B::vs() 读父 private static 放行）；消息同报声明类 */
        {
            TClass *sowner = sp->decl ? sp->decl : cls;
            if (!tphp_vis_ok(sp->visibility, sowner, frame_scope_cls(vm)))
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot access %s property %s::$%s",
                         vis_name(sp->visibility), tstr_val(sowner->name),
                         tstr_val(cname) + 1);
        }
        TVal r = sprop_slot(cls, sp)[0];
        tval_addref(r);
        wtake(vm, op, r);
        return 1;
    }
    TClass *cown = NULL;
    int cidx = -1;
    if (tphp_class_has_const(cls, tstr_val(cname), &out, &cown, &cidx)) {
        /* P2-B: 惰性常量表达式——首次取用求值并缓存到 owner 的槽位。
         * cvals 的 T_UNDEF 是 thunk 占位标记（折叠过的 null 存 T_NULL）。 */
        if (cown && cown->cthunks && cidx >= 0 &&
            cown->cthunks[cidx] && out.t == T_UNDEF) {
            if (const_eval_in_progress(cown, cidx))
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot declare self-referencing constant %s::%s",
                         tstr_val(cown->name), tstr_val(cname));
            const_eval_push(cown, cidx);
            out = tphp_call_thunk(vm, cown->cthunks[cidx]);
            const_eval_pop();
            cown->cvals[cidx] = out;      /* 类接管 thunk 返回值引用 */
            tval_addref(out);
        } else {
            tval_addref(out);
        }
        /* PHP 8.3 typed class constants：按声明类型校验（含 thunk 求值结果） */
        if (cown && cown->ctypes && cidx >= 0 && cown->ctypes[cidx]) {
            const char *given = NULL;
            if (!tphp_type_accepts(tstr_val(cown->ctypes[cidx]), out,
                                   (TClass *)cown, &given))
                vm_raise(vm, ex_class("TypeError"), op,
                         "Typed constant %s::%s must be of type %s, %s given",
                         tstr_val(cown->name), tstr_val(cname),
                         tstr_val(cown->ctypes[cidx]), given ? given : "?");
        }
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

/* C1+既有缺口: isset(C::$prop / C::$$v / C::CONST) —— 未声明/不可见/
 * 未初始化静默 false（对齐 PHP；此前 A_ISSET 没有静态分支、连字面
 * isset(C::$x) 都是加载期报错）。操作数布局与 op_fetch_cconst 一致。 */
static int32_t op_isset_cconst(VM *vm, TOp *op) {
    int kind;
    TStr *key;
    if (op->t1 != OPD_UNUSED) {
        kind = (int)op->ext;
        TStr *mn = op_member_name(vm, op, 1);
        TStr *pf = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
        key = dyn_sprop_key(kind, tstr_val(pf), mn);
    } else {
        kind = (int)op->a1;
        key = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    }
    TClass *cls;
    TStr *cname;
    if (kind == 3) {
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
        extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
        cls = class_resolve_lc2(clc, tstr_val(tstr_intern(s, cl)));
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class \"%.*s\" not found",
                     (int)cl, s);
        cname = tstr_intern(sep + 2, strlen(sep + 2));
    } else {
        cls = smcall_cls(vm, op, kind, NULL);
        cname = key;
    }
    bool set = false;
    tphp_class_ensure_flat(cls);
    if (tstr_val(cname)[0] == '$') {
        TProp *sp = tphp_class_find_sprop(cls, tstr_val(cname) + 1);
        if (sp && tphp_vis_ok(sp->visibility, sp->decl ? sp->decl : cls,
                              frame_scope_cls(vm))) {
            TVal v = sprop_slot(cls, sp)[0];
            set = v.t != T_UNDEF;
        }
    } else {
        TVal out;
        set = tphp_class_has_const(cls, tstr_val(cname), &out, NULL, NULL);
    }
    if (op->tr != OPD_UNUSED) {
        TVal *d = &vm->x.frame->slots[op->ar];
        tval_release(d);
        d[0] = set ? TV_TRUE : TV_FALSE;
    }
    return 1;
}

/* static property write: C::$prop = v / self::$prop = v
 * operands: o1=kind (0 self/1 parent/2 static/3 FQ), o2=key lit, o3=value */
static int32_t op_assign_sprop(VM *vm, TOp *op) {
    int kind;
    TStr *key;
    if (op->t1 != OPD_UNUSED) {
        /* C1: C::$$v = x 动态（同 op_fetch_cconst 布局） */
        kind = (int)op->ext;
        TStr *mn = op_member_name(vm, op, 1);
        TStr *pf = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
        key = dyn_sprop_key(kind, tstr_val(pf), mn);
    } else {
        kind = (int)op->a1;
        key = (TStr *)vm->x.frame->fn->lits[op->a2].u.p;
    }
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
        extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
        cls = class_resolve_lc2(clc, tstr_val(tstr_intern(s, cl)));
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class \"%.*s\" not found",
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
    /* PHP 8.4 asymmetric visibility：静态属性写入按 set 可见性（不回退） */
    if (sp->set_visibility && sp->set_visibility != sp->visibility) {
        TClass *owner = sp->decl ? sp->decl : cls;
        if (!asym_set_ok(sp->set_visibility, owner, frame_scope_cls(vm))) {
            char sb[160];
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot modify %s(set) property %s::$%s from %s",
                     vis_name(sp->set_visibility), tstr_val(owner->name),
                     tstr_val(pname) + 1, asym_scope(vm, sb, sizeof sb));
        }
    }
    {
        TClass *sowner = sp->decl ? sp->decl : cls;
        if (!tphp_vis_ok(sp->visibility, sowner, frame_scope_cls(vm)))
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot access %s property %s::$%s",
                     vis_name(sp->visibility), tstr_val(sowner->name),
                     tstr_val(pname) + 1);
    }
    TVal val = rdv(vm, op, 3);
    /* P1-1: typed 静态属性赋值校验 */
    if (sp->type) {
        const char *given = NULL;
        if (!tphp_type_accepts(tstr_val(sp->type), val, cls, &given))
            vm_raise(vm, ex_class("TypeError"), op,
                     "Cannot assign %s to property %s::$%s of type %s",
                     given ? given : "?",
                     tstr_val(cls->name), tstr_val(pname) + 1,
                     tstr_val(sp->type));
    }
    tval_addref(val);
    TVal *slot = sprop_slot(cls, sp);
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

/* ---- P2-B: $obj::$prop / $className::$prop（实例或类名串访问静态属性） ---- */

/* base → 类：对象取其类；字符串按类名解析；其余抛错（对齐 PHP：
 * "Class name must be a valid object or a string"） */
static TClass *osprop_cls(VM *vm, TOp *op, TVal base) {
    if (base.t == T_OBJ && ((GCHead *)base.u.p)->kind == GC_KIND_OBJ)
        return ((TObj *)base.u.p)->cls;
    if (base.t == T_STR) {
        TStr *s = (TStr *)base.u.p;
        char lc[128];
        size_t n = s->len < sizeof lc - 1 ? s->len : sizeof lc - 1;
        for (size_t i = 0; i < n; i++) {
            char ch = s->c[i];
            lc[i] = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        }
        lc[n] = 0;
        extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
        TClass *cls = class_resolve_lc2(lc, s->c);
        if (!cls)
            vm_raise(vm, ex_class("Error"), op, "Class \"%s\" not found", s->c);
        return cls;
    }
    vm_raise(vm, ex_class("Error"), op,
             "Class name must be a valid object or a string");
    return NULL;
}

/* $obj::$prop read（与 C::$prop 同语义：flat 查找/可见性/typed） */
static int32_t op_fetch_osprop(VM *vm, TOp *op) {
    TVal base = rdv(vm, op, 1);
    TStr *pname = op_member_name_os(vm, op);   /* C1 */
    TClass *cls = osprop_cls(vm, op, base);
    tphp_class_ensure_flat(cls);
    TProp *sp = tphp_class_find_sprop(cls, tstr_val(pname) + 1);
    if (!sp)
        vm_raise(vm, ex_class("Error"), op,
                 "Access to undeclared static property %s::$%s",
                 tstr_val(cls->name), tstr_val(pname) + 1);
    {
        TClass *sowner = sp->decl ? sp->decl : cls;
        if (!tphp_vis_ok(sp->visibility, sowner, frame_scope_cls(vm)))
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot access %s property %s::$%s",
                     vis_name(sp->visibility), tstr_val(sowner->name),
                     tstr_val(pname) + 1);
    }
    TVal r = sprop_slot(cls, sp)[0];
    tval_addref(r);
    wtake(vm, op, r);
    return 1;
}

/* $obj::$prop = v */
static int32_t op_assign_osprop(VM *vm, TOp *op) {
    TVal base = rdv(vm, op, 1);
    TStr *pname = op_member_name_os(vm, op);   /* C1 */
    TClass *cls = osprop_cls(vm, op, base);
    tphp_class_ensure_flat(cls);
    TProp *sp = tphp_class_find_sprop(cls, tstr_val(pname) + 1);
    if (!sp)
        vm_raise(vm, ex_class("Error"), op,
                 "Access to undeclared static property %s::$%s",
                 tstr_val(cls->name), tstr_val(pname) + 1);
    /* PHP 8.4 asymmetric visibility：静态属性写入按 set 可见性（不回退） */
    if (sp->set_visibility && sp->set_visibility != sp->visibility) {
        TClass *owner = sp->decl ? sp->decl : cls;
        if (!asym_set_ok(sp->set_visibility, owner, frame_scope_cls(vm))) {
            char sb[160];
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot modify %s(set) property %s::$%s from %s",
                     vis_name(sp->set_visibility), tstr_val(owner->name),
                     tstr_val(pname) + 1, asym_scope(vm, sb, sizeof sb));
        }
    }
    {
        TClass *sowner = sp->decl ? sp->decl : cls;
        if (!tphp_vis_ok(sp->visibility, sowner, frame_scope_cls(vm)))
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot access %s property %s::$%s",
                     vis_name(sp->visibility), tstr_val(sowner->name),
                     tstr_val(pname) + 1);
    }
    TVal val = rdv(vm, op, 3);
    /* P1-1: typed 静态属性赋值校验 */
    if (sp->type) {
        const char *given = NULL;
        if (!tphp_type_accepts(tstr_val(sp->type), val, cls, &given))
            vm_raise(vm, ex_class("TypeError"), op,
                     "Cannot assign %s to property %s::$%s of type %s",
                     given ? given : "?",
                     tstr_val(cls->name), tstr_val(pname) + 1,
                     tstr_val(sp->type));
    }
    tval_addref(val);
    TVal *slot = sprop_slot(cls, sp);
    tval_release(slot);
    *slot = val;
    return 1;
}

/* isset($obj::$prop)：未声明/不可见/未初始化静默 false；非法基类名仍抛错
 * （对齐 PHP isset 语义，探针见 TODO-P2-P3 交接） */
static int32_t op_isset_osprop(VM *vm, TOp *op) {
    TVal base = rdv(vm, op, 1);
    TStr *pname = op_member_name_os(vm, op);   /* C1 */
    TClass *cls = osprop_cls(vm, op, base);
    tphp_class_ensure_flat(cls);
    TProp *sp = tphp_class_find_sprop(cls, tstr_val(pname) + 1);
    if (!sp || !tphp_vis_ok(sp->visibility, sp->decl ? sp->decl : cls,
                            frame_scope_cls(vm))) {
        wtake(vm, op, TV_FALSE);
        return 1;
    }
    TVal v = sprop_slot(cls, sp)[0];
    wtake(vm, op, (v.t != T_UNDEF && v.t != T_NULL) ? TV_TRUE : TV_FALSE);
    return 1;
}

/* unset($obj->prop): a declared slot becomes UNDEF (isset flips false);
 * a missing slot forwards to __unset(name) */
static int32_t op_unset_prop(VM *vm, TOp *op) {
    TVal objv = rdv(vm, op, 1);
    TStr *name = op_member_name(vm, op, 2);   /* C1 */
    if (objv.t != T_OBJ || ((GCHead *)objv.u.p)->kind != GC_KIND_OBJ)
        return 1;                         /* non-user objects: no-op */
    TObj *o = (TObj *)objv.u.p;
    int slot = tphp_class_find_prop(o->cls, tstr_val(name));
    if (slot >= 0) {
        /* PHP 8.4 asymmetric visibility：unset 也按 set 可见性（不回退） */
        TProp *ap = &o->cls->props[slot];
        if (ap->set_visibility && ap->set_visibility != ap->visibility) {
            TClass *owner = ap->decl ? ap->decl : o->cls;
            if (!asym_set_ok(ap->set_visibility, owner, frame_scope_cls(vm))) {
                char sb[160];
                vm_raise(vm, ex_class("Error"), op,
                         "Cannot unset %s(set) property %s::$%s from %s",
                         vis_name(ap->set_visibility), tstr_val(owner->name),
                         tstr_val(name), asym_scope(vm, sb, sizeof sb));
            }
        }
        if (!tphp_vis_ok(o->cls->props[slot].visibility, prop_owner(o, slot),
                         frame_scope_cls(vm))) {
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
        /* PHP 8.4：任一钩子存在 → 禁止 unset（会绕过 set 钩子） */
        if (ap->has_hooks)
            vm_raise(vm, ex_class("Error"), op,
                     "Cannot unset hooked property %s::$%s",
                     tstr_val(o->cls->name), tstr_val(name));
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
static int32_t op_fcc_method(VM *vm, TOp *op) {
    /* P2-A $obj->m(...): bound method closure — TClosure with fn=NULL,
     * receiver + method name in captures, GC_FLAG_MFCC set; op_init_dyn
     * dispatches it like a method call */
    TVal ov = rdv(vm, op, 1);
    if (ov.t != T_OBJ)
        vm_raise(vm, ex_class("Error"), op,
                 "Cannot create a first-class method callable from a non-object");
    TStr *mname = op_member_name(vm, op, 2);   /* C1: $obj->$m(...) */
    TVal caps[2];
    caps[0] = ov;
    caps[1] = TV_STR(mname);
    tval_addref(caps[1]);
    TClosure *clo = tclo_alloc(NULL, caps, 2);
    clo->h.flags |= GC_FLAG_MFCC;
    wtake(vm, op, TV_CLO(clo));
    return 1;
}

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
    if (l.t == T_CLOSURE) {
        /* P1-3: 闭包值 instanceof Closure */
        res = lc_eq(tstr_val(cname), "Closure");
    } else if (l.t == T_OBJ) {
        GCHead *h = (GCHead *)l.u.p;
        if (h->kind == GC_KIND_OBJ) {
            char lc[128];
            const char *s = tstr_val(cname);
            size_t k = 0;
            for (; s[k] && k < sizeof lc - 1; k++)
                lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
            lc[k] = 0;
            extern TClass *class_resolve_lc2(const char *fqcn_lc, const char *orig_name);
            TClass *b = class_resolve_lc2(lc, tstr_val(cname));
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
    TStr *name = op_member_name(vm, op, 2);   /* C1 */
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
        /* 可见性优先（对齐 PHP：不可见 → __get，与有无钩子无关） */
        if (!tphp_vis_ok(o->cls->props[slot].visibility, prop_owner(o, slot),
                         frame_scope_cls(vm))) {
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
        /* ---- PHP 8.4 property hooks：读路径（可见性之后） ---- */
        {
            TProp *pp = &o->cls->props[slot];
            if (pp->has_hooks && !in_hook_backing(vm, o, pp->name)) {
                if (pp->hook_get) {
                    TVal out;
                    if (call_hook(vm, op, o, pp->name, pp->hook_get,
                                  NULL, 0, &out)) {
                        wtake(vm, op, out);
                        return 1;
                    }
                }
                if (pp->is_virtual)
                    vm_raise(vm, ex_class("Error"), op,
                             "Cannot read virtual property %s::$%s without get hook",
                             tstr_val(o->cls->name), tstr_val(name));
                /* 有 backing 且无 get 钩子 → 落回默认读 */
            }
            /* 虚拟属性 backing 槽恒为 UNDEF，跳过 T_UNDEF 检查 */
            if (!pp->is_virtual && o->props[slot].t == T_UNDEF)
                vm_raise(vm, ex_class("UndefinedError"), op,
                         "Typed property %s::$%s must not be accessed before initialization",
                         tstr_val(o->cls->name), tstr_val(name));
        }
        TVal out = o->props[slot];
        tval_addref(out);
        wtake(vm, op, out);
        return 1;
    }
    if (h->kind != GC_KIND_EX)
        vm_raise(vm, ex_class("Error"), op,
                 "Attempt to read property \"%s\" on object", tstr_val(name));
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
    vm_unwind_jump(vm);
}

/* ---- unwinding ---- */
/* P1-2: catch 匹配按名字跨两个世界——引擎异常走 TExClass 链；用户异常类
 * 实例（ex->wrap）走 TClass 链（含接口）。catch_cls 索引仅留作builtin
 * 快路的备注，匹配一律以 catch_nm 为准。 */
static bool catch_matches(const TExEntry *e, const TExObj *ex) {
    for (int i = 0; i < e->n_catches; i++) {
        const char *want = e->catch_nm ? tstr_val(e->catch_nm[i]) : NULL;
        if (!want) continue;
        if (ex->wrap) {
            char lc[256];
            size_t k = 0;
            for (const char *s = want; s[k] && k + 1 < sizeof lc; k++)
                lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
            lc[k] = 0;
            TClass *wc = tphp_class_lookup(lc);
            if (wc && tphp_class_isa(ex->wrap->cls, wc)) return true;
        }
        for (const TExClass *c = ex->cls; c; c = c->parent)
            if (tstr_case_eq(c->name, want)) return true;
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
    const_eval_reset();           /* P2-B: unwind 中断的常量求值允许重试 */
    vm->x.n_trace = 0;
    /* B6: 跨帧归因修正——帧被弹出说明异常来自其 callee，而调用方 pc 已在
     * 压帧时前推过调用点（op_do_fcall/op_new_user 的 pc++），归因必须落回
     * 调用点 op 自身：否则"调用点后紧跟 try"时 pc == 下一个 try 的
     * start_pc，会被误判进其保护区（PHP 归因到调用点、不捕获）。
     * thunk 例外：tphp_call_thunk 不前推 caller pc（pc 仍停在调用 handler
     * 上），弹 thunk 帧后不减。首个帧是抛出点本身，pc 语义本就正确。 */
    uint32_t pc_back = 0;
    while (vm->x.frame) {
        VMFrame *f = vm->x.frame;
        TFunc *fn = f->fn;
        uint32_t pc = f->pc;
        if (pc > pc_back) pc -= pc_back;
        if (vm->x.n_trace < 64 && fn->n_ops && pc < fn->n_ops) {
            vm->x.trace[vm->x.n_trace].fn =
                fn->name ? tstr_val(fn->name) : "{closure}";
            vm->x.trace[vm->x.n_trace].line = fn->ops[pc].line;
            vm->x.n_trace++;
        }
        /* innermost try region first; within it, catch clauses in
         * declaration order (multiple catch blocks share the region) */
        uint32_t best_end = UINT32_MAX;
        for (int i = 0; i < fn->n_ex; i++) {
            TExEntry *e = &fn->ex[i];
            if (pc >= e->start_pc && pc < e->end_pc &&
                e->end_pc < best_end)
                best_end = e->end_pc;
        }
        TExEntry *hit = NULL;
        if (best_end != UINT32_MAX) {
            for (int i = 0; i < fn->n_ex; i++) {
                TExEntry *e = &fn->ex[i];
                if (pc >= e->start_pc && pc < e->end_pc &&
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
        /* 构造器执行中抛异常：半成品对象不落地，随帧一起释放（PHP 语义：
         * ctor throw → 对象销毁）。ctor rec 位于 at_entry-1（op_new_user
         * 先 push rec 再建帧），slot0 的 $this 是最后一个引用。 */
        VMCallRec *crec = (f->n_calls_at_entry > 0)
            ? &vm->x.calls[f->n_calls_at_entry - 1] : NULL;
        /* B6: 下一轮（父帧）的归因修正量——thunk 不前推 caller pc；
         * 生成器帧同理（resume 无 call rec，误取邻 rec 会把 caller pc
         * 推出 try 范围 → 本该接住的异常变 Uncaught） */
        pc_back = (crec && crec->is_thunk) || f->gen ? 0u : 1u;
        TObj *half_obj = NULL;
        if (crec && crec->is_ctor && f->slots[0].t == T_OBJ &&
            ((GCHead *)f->slots[0].u.p)->kind == GC_KIND_OBJ)
            half_obj = (TObj *)f->slots[0].u.p;
        vm->x.n_calls = f->n_calls_at_entry;
        if (vm->x.n_calls > 0) vm->x.n_calls--;
        vm->x.n_args = (int)f->args_base;
        vm->x.frame = f->prev;
        frame_destroy(f);
        if (half_obj) {
            /* 构造失败的对象不跑 __destruct（PHP 语义：ctor 抛出即从未
             * 完成构造）；DTORED 标记让 tobj_free 直连释放路径 */
            half_obj->h.flags |= GC_FLAG_DTORED;
            TVal ov = TV_OBJ(half_obj);
            tval_release(&ov);        /* 帧销毁已释放槽内引用，此处收尾 base */
        }
    }
    /* bottom reached: uncaught。main 协程就地打印 fatal 并定退出码；
     * 子协程（coro_result != NULL）静默收尾——异常保留在执行上下文，
     * 由 await 透传，或在脚本结束时由调度器统一报 fatal（M4 sched.c） */
    vm->x.done = true;
    vm->x.uncaught = true;
    if (!vm->x.coro_result) {
        /* Uncaught 报告的文件 = 抛出点帧的源文件（ex->file，PHP 对拍：
         * "in Base.php:19" 而非主脚本）；旧异常无记录时回落主 prog */
        const char *file = ex->file ? ex->file
                         : (vm->prog->file ? vm->prog->file : "-");
        const char *cls_name = ex->wrap && ex->wrap->cls->name
            ? tstr_val(ex->wrap->cls->name) : ex->cls->name;
        fprintf(stderr, "PHP Fatal error:  Uncaught %s: %s in %s:%d\n",
                cls_name, tstr_val(ex->message), file, ex->line);
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
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->vis_scope = (fn->is_method && fn->decl_cls)
        ? (TClass *)fn->decl_cls : NULL;
    f->called_cls = NULL;             /* thunk 帧无类作用域（补未初始化） */
    f->fn = fn;
    f->file = fn->file;
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
     * dispatch loop advances pc by that handler's return value.
     * deep 语义（跑到本帧弹出）：默认值/常量 thunk 允许 `new` 与函数调用
     * （PHP 8.1 new-in-initializers）——子帧（ctor 等）必须在本循环内继续
     * 执行；旧"帧切换即退出"会让 ctor 帧成孤儿（$this 为 null）。 */
    vm->x.frame = f;
    static int thunk_trace = -1;
    if (thunk_trace < 0) thunk_trace = getenv("TPHP_TRACE") ? 1 : 0;
    VMFrame *stop = f->prev;   /* f 弹出即 free，勿在条件里读 f->prev（UAF） */
    while (vm->x.frame && vm->x.frame != stop) {
        VMFrame *cf = vm->x.frame;
        TOp *op = &cf->fn->ops[cf->pc];
        if (thunk_trace) fprintf(stderr, "[trace %s pc=%u %s]\n", tstr_val(cf->fn->name),
                                 cf->pc, tphp_op_name(op->op));
        int32_t d = ((OpHandler)op->handler)(vm, op);
        if (d) cf->pc += (uint32_t)d;
    }
    return vm->x.thunk_ret;
}

/* ---- 运行期 require/include（动态路径 OP_REQUIRE） ---- */

/* 运行期 once 去重 + 展开防环（per-thread） */
static _Thread_local char **g_rt_once;
static _Thread_local int g_n_rt_once, g_cap_rt_once;
static _Thread_local char **g_rt_busy;
static _Thread_local int g_n_rt_busy, g_cap_rt_busy;

static bool rt_list_has(char **list, int n, const char *s) {
    for (int i = 0; i < n; i++)
        if (strcmp(list[i], s) == 0) return true;
    return false;
}

static void rt_list_push(char ***list, int *n, int *cap, const char *s) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *list = tm_realloc(*list, sizeof(char *) * (size_t)*cap);
    }
    (*list)[*n] = tm_malloc(strlen(s) + 1);
    strcpy((*list)[*n], s);
    (*n)++;
}

static const char *rt_dir_of(const char *path) {
    static char dir[1024];
    const char *slash = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    const char *s = slash > bslash ? slash : bslash;
    if (!s) return NULL;
    size_t n = (size_t)(s - path);
    if (n >= sizeof dir) n = sizeof dir - 1;
    memcpy(dir, path, n);
    dir[n] = 0;
    return dir;
}

/* 拼绝对/相对路径；dir 为 NULL 时返回 rel 副本 */
static char *rt_join(const char *dir, const char *rel) {
    if (!dir || !dir[0]) {
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

static char *rt_read_file(const char *path) {
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
    return buf;
}

/* 运行期读被包含源码：bundle → 磁盘 → pack zip */
static char *rt_load_source(const char *full, const char *rel,
                            const char **out_name) {
    char bname[128];
    tphp_bundle_name_of(rel, bname, sizeof bname);
    const TBundle *bun = tphp_bundle_find(bname);
    if (bun) {
        *out_name = bun->name;
        char *cp = tm_malloc(bun->len + 1);
        memcpy(cp, bun->src, bun->len);
        cp[bun->len] = 0;
        return cp;
    }
    char *src = full ? rt_read_file(full) : NULL;
    if (src) {
        *out_name = full;
        return src;
    }
    /* pack zip：source_root 剥离 + 原样 rel */
    if (tphp_pack_has_bundle()) {
        void *data = NULL;
        size_t len = 0;
        const char *root = tphp_pack_source_root();
        if (root && root[0] && full) {
            size_t rn = strlen(root);
            while (rn > 0 && (root[rn - 1] == '/' || root[rn - 1] == '\\')) rn--;
            if (strncmp(full, root, rn) == 0 &&
                (full[rn] == '/' || full[rn] == '\\')) {
                const char *p = full + rn;
                while (*p == '/' || *p == '\\') p++;
                if (p[0] && tphp_pack_read(p, &data, &len)) {
                    char *cp = tm_malloc(len + 1);
                    memcpy(cp, data, len);
                    cp[len] = 0;
                    free(data);
                    *out_name = full;
                    return cp;
                }
            }
        }
        char key[1024];
        snprintf(key, sizeof key, "%s", rel);
        for (char *q = key; *q; q++) if (*q == '\\') *q = '/';
        if (tphp_pack_read(key, &data, &len)) {
            char *cp = tm_malloc(len + 1);
            memcpy(cp, data, len);
            cp[len] = 0;
            free(data);
            *out_name = full ? full : rel;
            return cp;
        }
    }
    return NULL;
}

/* OP_REQUIRE：t1=path 字符串；ext bit0=is_require bit1=is_once；tr/ar=结果
 * 成功时压入被包含文件 main 帧（is_include），RETURN 后结果写入 dst。 */
static int32_t op_require(VM *vm, TOp *op) {
    TVal pv = rdv(vm, op, 1);
    if (pv.t != T_STR)
        vm_raise(vm, ex_class("TypeError"), op,
                 "require/include expects string path, %s given",
                 tphp_val_type_name(pv));
    const char *rel = tstr_val((TStr *)pv.u.p);
    bool is_req = (op->ext & 1) != 0;
    bool is_once = (op->ext & 2) != 0;

    /* once 去重用 rel 原样（与语句级内联一致；不强制 realpath） */
    if (is_once && rt_list_has(g_rt_once, g_n_rt_once, rel)) {
        wtake(vm, op, TV_INT(1));
        return 1;
    }
    if (rt_list_has(g_rt_busy, g_n_rt_busy, rel)) {
        if (is_req)
            vm_raise(vm, ex_class("Error"), op,
                     "Circular require/include of '%s'", rel);
        wtake(vm, op, TV_FALSE);
        return 1;
    }

    const char *cur_file = vm->x.frame->file
        ? vm->x.frame->file
        : (vm->prog && vm->prog->file ? vm->prog->file : NULL);
    char *full = NULL;
    if (rel[0] != '/' && !(rel[0] && rel[1] == ':')) {
        const char *dir = cur_file ? rt_dir_of(cur_file) : NULL;
        full = rt_join(dir, rel);
    } else {
        full = rt_join(NULL, rel);
    }

    const char *parse_name = NULL;
    char *src = rt_load_source(full, rel, &parse_name);
    if (!src) {
        tm_free(full);
        if (is_req)
            vm_raise(vm, ex_class("Error"), op,
                     "Failed opening required file '%s'", rel);
        wtake(vm, op, TV_FALSE);
        return 1;
    }

    rt_list_push(&g_rt_busy, &g_n_rt_busy, &g_cap_rt_busy, rel);

    /* parse + compile；parse_name 指向 full 或 intern/bundle 名，需稳定 */
    char *name_copy = tm_malloc(strlen(parse_name) + 1);
    strcpy(name_copy, parse_name);
    AStmtList *ast = tphp_parse_script(src, name_copy);
    tm_free(src);
    Program *inc = tphp_compile(ast, name_copy);
    /* 类已 register；不 freeze（lazy flatten；跨线程共享后置） */

    /* 挂到 VM 的 include 程序表（TFunc 生命周期） */
    vm->inc_progs = tm_realloc(vm->inc_progs,
                               sizeof(Program *) * (size_t)(vm->n_inc_progs + 1));
    vm->inc_progs[vm->n_inc_progs++] = inc;

    if (is_once) rt_list_push(&g_rt_once, &g_n_rt_once, &g_cap_rt_once, rel);
    g_n_rt_busy--;   /* pop busy */

    /* 压 include main 帧：RETURN 经 call rec 写 dst；is_include 空值→1 */
    VMCallRec *rec = call_push(vm, op);
    rec->uf = inc->main;
    rec->dst_kind = op->tr;
    rec->dst_slot = op->ar;
    rec->is_include = true;

    VMFrame *f = tm_malloc(sizeof *f);
    memset(f, 0, sizeof *f);   /* zero gen/… */
    f->fn = inc->main;
    f->file = name_copy;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = rec->args_base;
    f->n_args_sent = 0;
    f->n_calls_at_entry = vm->x.n_calls;
    uint32_t nslots = (uint32_t)inc->main->n_locals + inc->main->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    vm->x.frame->pc++;
    vm->x.frame = f;
    tm_free(full);
    return 0;   /* 新帧从 pc=0 起跑 */
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
    H(op_assign_sprop), H(op_unset_prop), H(op_clone), H(op_fcc_method),
    H(op_fetch_osprop), H(op_assign_osprop), H(op_isset_osprop),
    H(op_init_new), H(op_new_late), H(op_new_dyn),
    H(op_isset_cconst),
    H(op_require),
    H(op_prophook),
    H(op_generator_create), H(op_yield),
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
        "ASSIGN_SPROP", "UNSET_PROP", "CLONE", "FCC_METHOD",
        "FETCH_OSPROP", "ASSIGN_OSPROP", "ISSET_OSPROP",
        "INIT_NEW", "NEW_LATE", "NEW_DYN",
        "ISSET_CCONST",
        "REQUIRE",
        "PROPHOOK",
        "GENERATOR_CREATE",
        "YIELD",
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
        if (vm->x.exception) vm_unwind_jump(vm);
        return;
    }
    VMFrame *f = tm_malloc(sizeof *f);
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->vis_scope = NULL;
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
    run_frame_until_pop_deep(vm, f);
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

/* 预置内置常量（M4：I/O 相关；define() 运行期走 op_define_const）。
 * 字符串常量用 tstr_intern（program-lifetime，不入 GC 注册表）。 */
static void vm_seed_consts(VM *vm) {
    static const struct { const char *name; const char *str; int64_t i; } seeds[] = {
        { "FILE_APPEND", NULL, 8 },
        { "LOCK_EX",     NULL, 2 },
        { "SEEK_SET",    NULL, 0 },
        { "SEEK_CUR",    NULL, 1 },
        { "SEEK_END",    NULL, 2 },
#ifdef _WIN32
        { "PHP_EOL",     "\r\n", 0 },
        { "DIRECTORY_SEPARATOR", "\\", 0 },
        { "PATH_SEPARATOR",      ";",  0 },
        { "PHP_OS_FAMILY", "Windows", 0 },
#elif defined(__APPLE__)
        { "PHP_EOL",     "\n",  0 },
        { "DIRECTORY_SEPARATOR", "/",  0 },
        { "PATH_SEPARATOR",      ":",  0 },
        { "PHP_OS_FAMILY", "Darwin", 0 },   /* 对齐 PHP：macOS 归 Darwin */
#elif defined(__linux__)
        { "PHP_EOL",     "\n",  0 },
        { "DIRECTORY_SEPARATOR", "/",  0 },
        { "PATH_SEPARATOR",      ":",  0 },
        { "PHP_OS_FAMILY", "Linux", 0 },
#else
        { "PHP_EOL",     "\n",  0 },
        { "DIRECTORY_SEPARATOR", "/",  0 },
        { "PATH_SEPARATOR",      ":",  0 },
        { "PHP_OS_FAMILY", "BSD", 0 },      /* FreeBSD/OpenBSD/NetBSD 等 */
#endif
        { "PHP_VERSION", TPHP_VERSION, 0 },
        { "PHP_INT_MAX", NULL, INT64_MAX },
        { "PHP_INT_MIN", NULL, INT64_MIN },
        /* 打包应用资源根（io 层 tphp://asset/ 前缀直读内存 zip）；
         * 未打包时读取会得到 "not a packed executable" 警告 */
        { "ASSET_ROOT",  "tphp://asset", 0 },
        /* preg_* 常量（PHP 值；PLAN/preg.md） */
        { "PREG_NO_ERROR", NULL, 0 },
        { "PREG_INTERNAL_ERROR", NULL, 1 },
        { "PREG_BACKTRACK_LIMIT_ERROR", NULL, 2 },
        { "PREG_RECURSION_LIMIT_ERROR", NULL, 3 },
        { "PREG_JIT_STACKLIMIT_ERROR", NULL, 4 },
        { "PREG_LIMIT_UTF8_ERROR", NULL, 5 },
        { "PREG_PATTERN_ORDER", NULL, 0 },
        { "PREG_SET_ORDER", NULL, 1 },
        { "PREG_OFFSET_CAPTURE", NULL, 256 },
        { "PREG_UNMATCHED_AS_NULL", NULL, 512 },
        { "PREG_SPLIT_NO_EMPTY", NULL, 1 },
        { "PREG_SPLIT_DELIM_CAPTURE", NULL, 2 },
        { "PREG_SPLIT_OFFSET_CAPTURE", NULL, 4 },
        { "PREG_GREP_INVERT", NULL, 1 },
    };
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        if (vm->n_consts >= 256) return;
        vm->consts[vm->n_consts].name = tstr_intern(seeds[i].name,
                                                    strlen(seeds[i].name));
        if (seeds[i].str)
            vm->consts[vm->n_consts].v = TV_STR(tstr_intern(seeds[i].str,
                                                            strlen(seeds[i].str)));
        else
            vm->consts[vm->n_consts].v = TV_INT(seeds[i].i);
        vm->n_consts++;
    }
    /* PHP_PACK：是否运行在打包 exe 内（tphp_pack_startup 已在 main 最
     * 早期跑过，此处查状态即可）。seeds 表只承载 str/int，bool 单独补。 */
    if (vm->n_consts < 256) {
        vm->consts[vm->n_consts].name = tstr_intern("PHP_PACK", 8);
        vm->consts[vm->n_consts].v =
            tphp_pack_has_bundle() ? TV_TRUE : TV_FALSE;
        vm->n_consts++;
    }
    /* PHP_UNAME：uname(2) 五字段关联数组（sysname/nodename/release/
     * version/machine），数据源 libuv uv_os_uname（跨平台，Windows 同样
     * 提供）。数组常量走 GC 无根算法：rc=1 超出内部引用和 → 外部可达；
     * op_fetch_const addref/release 配平，表内这份永不到 0。
     * 字符串键值全 intern，不入 GC 注册表。 */
    if (vm->n_consts < 256) {
        uv_utsname_t un;
        if (uv_os_uname(&un) == 0) {
            /* libuv 的 uv_utsname_t 特意不含 nodename（其文档称无意义），
             * 但 PHP_UNAME 按用户约定保留该字段 → uv_os_gethostname 补齐 */
            char host[256] = "unknown";
            size_t hsz = sizeof host;
            uv_os_gethostname(host, &hsz);
            static const char *keys[5] = {
                "sysname", "nodename", "release", "version", "machine",
            };
            const char *vals[5] = {
                un.sysname, host, un.release, un.version, un.machine,
            };
            TArr *ua = tarr_new();
            for (int i = 0; i < 5; i++) {
                tarr_set(ua,
                         TV_STR(tstr_intern(keys[i], strlen(keys[i]))),
                         TV_STR(tstr_intern(vals[i], strlen(vals[i]))));
            }
            vm->consts[vm->n_consts].name = tstr_intern("PHP_UNAME", 9);
            vm->consts[vm->n_consts].v = TV_ARR(ua);
            vm->n_consts++;
        }
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

/* ---- __destruct (P0-2, 2026-10-15) ----
 * 释放点（tobj_free，rc 归零）与请求收尾都会走到这里。语义对齐 PHP：
 *  - 每个对象最多析构一次（GC_FLAG_DTORED，调用方设置）
 *  - 析构器内抛异常 = 就地 fatal（exit 255）：PHP 同样不让析构异常从
 *    释放点向外传播——释放点可能在 op_return 的槽清理或 GC sweep 中途，
 *    此时 longjmp 会破坏半释放的帧/快照，必须就地设防
 *  - 局部 setjmp 换掉 vm->x.unwind：vm_call_magic 的 raise 落到这里，
 *    不会窜到已死 fiber 的外圈 setjmp（收尾路径尤其危险） */
void tphp_obj_run_dtor(TObj *o) {
    if (o->cls->kind == CK_ENUM) return;               /* 枚举单例不析构 */
    if (o->cls->m_dtor < 0 || o->cls->m_dtor >= o->cls->n_methods) return;
    TMethod *m = &o->cls->methods[o->cls->m_dtor];
    if (!m->fn || m->is_abstract) return;
    VM *vm = tphp_vm_instance();
    jmp_buf saved;
    memcpy(saved, vm->x.unwind, sizeof(jmp_buf));
    memset(&g_magic_stub, 0, sizeof g_magic_stub);
    if (setjmp(vm->x.unwind) == 0) {
        TVal out;
        if (vm_call_magic(vm, &g_magic_stub, o, o->cls, "__destruct",
                          NULL, 0, &out))
            tval_release(&out);
        memcpy(vm->x.unwind, saved, sizeof(jmp_buf));
    } else {
        TExObj *ex = vm->x.exception;
        const char *file = (ex && ex->file) ? ex->file
                         : ((vm->prog && vm->prog->file) ? vm->prog->file : "-");
        fprintf(stderr, "PHP Fatal error:  Uncaught %s: %s in %s:%d\n",
                (ex && ex->cls) ? ex->cls->name : "Error",
                (ex && ex->message) ? tstr_val(ex->message) : "",
                file, ex ? ex->line : 0);
        fflush(stdout);
        exit(255);
    }
}

/* 请求收尾：对全部带 __destruct 的存活对象跑一遍。逆创建序近似——
 * gc_list 头插队，头部即最新。快照引用防析构器相互释放导致 UAF；
 * DTORED 先标记防止 tobj_free 路径重入。 */
void tphp_run_shutdown_dtors(void) {
    VM *vm = tphp_vm_instance();
    /* tphp_sched_run 把宿主 ctx memset 清零过（数组指针为 NULL）——收尾
     * 析构要跑 PHP，先确保 per-context 堆数组存在 */
    tphp_ctx_ensure(&vm->x);
    vm->x.exception = NULL;      /* 未消费异常已由 sched_report_uncaught 报过 */
    /* B4: 先按类声明序释放静态属性（级联触发 __destruct，对齐 PHP
     * shutdown 析构序）；随后快照循环兜底环中/其它持有者对象 */
    tphp_class_release_sprops();
    TObj *list[1024];
    int n = 0;
    for (GCHead *h = tphp_gc_registry(); h && n < 1024; h = h->gc_next) {
        if (h->kind != GC_KIND_OBJ) continue;
        TObj *o = (TObj *)h;
        if (o->cls->kind == CK_ENUM) continue;
        if (o->cls->m_dtor < 0 || o->cls->m_dtor >= o->cls->n_methods) continue;
        if (h->flags & GC_FLAG_DTORED) continue;
        h->flags |= GC_FLAG_DTORED;
        h->rc++;                 /* 快照引用 */
        list[n++] = o;
    }
    for (int i = 0; i < n; i++)
        tphp_obj_run_dtor(list[i]);
    for (int i = 0; i < n; i++) {
        TVal self = TV_OBJ(list[i]);
        tval_release(&self);     /* 退快照引用；rc 归零则直连释放 */
    }
    dyn_global_release_all();    /* B2: 动态全局条目收尾（幂等） */
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
 * closure（含 first-class builtin 包装）、函数名字符串、数组 callable
 * [$obj,'m']/['Cls','m']、'Cls::m' 字符串、绑定方法闭包（MFCC）。
 * 返回 +1 值；异常沿正常 unwind 传播。 */
/* PHP callable 判定（is_callable / need_callable 共用）。
 * 与 tphp_vm_call_callable 的分派分支保持同构：这里判 true 的形态，
 * 调用点必须能真正调起来（否则 is_callable 会撒谎）。 */
bool tphp_vm_is_callable(TVal v) {
    switch (v.t) {
    case T_CLOSURE:
        return true;
    case T_STR:
        return true;                  /* 函数名 / 'Cls::m'：粗判，调用点解析 */
    case T_ARR: {
        TArr *a = (TArr *)v.u.p;
        if (a->n != 2) return false;
        TVal e[2];
        int k = 0;
        for (uint32_t i = 0; i < a->dused && k < 2; i++)
            if (a->data[i].t != T_UNDEF) e[k++] = a->data[i];
        if (k != 2 || e[1].t != T_STR) return false;
        return e[0].t == T_OBJ || e[0].t == T_STR;
    }
    case T_OBJ: {
        TObj *o = (TObj *)v.u.p;
        if (o->h.kind == GC_KIND_EX || !o->cls) return false;   /* 异常对象 */
        tphp_class_ensure_flat(o->cls);
        TMethod *m = tphp_class_find_method(o->cls, "__invoke");
        return m && (m->fn || m->native) && !m->is_abstract && !m->is_static;
    }
    default:
        return false;
    }
}

TVal tphp_vm_call_callable(VM *vm, TVal callable, TVal *args, int n) {
    TClosure *clo = NULL;
    BuiltinFn bfn = NULL;
    TFunc *fn = NULL;
    NativeMethodFn nat = NULL;
    void *nat_self = NULL;       /* receiver for native methods */
    TObj *recv = NULL;           /* $this for a user-method frame */
    TClass *mcls = NULL;         /* class scope for the method frame */
    if (callable.t == T_CLOSURE) {
        clo = (TClosure *)callable.u.p;
        if (clo->bfn) bfn = (BuiltinFn)clo->bfn;
        else if (clo->fn == NULL && (clo->h.flags & GC_FLAG_MFCC)) {
            /* P2-A bound method closure ($obj->m(...)) */
            recv = (TObj *)clo->captures[0].u.p;
            TStr *mname = (TStr *)clo->captures[1].u.p;
            char mlc[128];
            { const char *s = tstr_val(mname);
              size_t k = 0; for (; s[k] && k < sizeof mlc - 1; k++)
                  mlc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
              mlc[k] = 0; }
            tphp_class_ensure_flat(recv->cls);
            TMethod *m = tphp_class_find_method(recv->cls, mlc);
            if (!m || (!m->fn && !m->native) || m->is_abstract)
                vm_raise(vm, ex_class("Error"), NULL,
                         "Method %s::%s() is not callable",
                         tstr_val(recv->cls->name), tstr_val(mname));
            mcls = recv->cls;
            if (m->fn) fn = m->fn;
            else { nat = (NativeMethodFn)m->native; nat_self = recv; }
        } else {
            fn = clo->fn;
        }
    } else if (callable.t == T_STR) {
        const char *s = tstr_val((TStr *)callable.u.p);
        const char *sep = NULL;
        /* the LAST separator: class names contain backslashes, never colons */
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2)
            sep = p;
        if (sep) {
            /* P2-B: 'Cls::m' string callable */
            size_t cl = (size_t)(sep - s);
            char clc[256];
            if (cl >= sizeof clc) cl = sizeof clc - 1;
            for (size_t i = 0; i < cl; i++)
                clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
            clc[cl] = 0;
            char mlc[128];
            { const char *s2 = sep + 2;
              size_t k = 0; for (; s2[k] && k < sizeof mlc - 1; k++)
                  mlc[k] = (s2[k] >= 'A' && s2[k] <= 'Z') ? (char)(s2[k] + 32) : s2[k];
              mlc[k] = 0; }
            extern TClass *class_resolve_lc(const char *fqcn_lc);
            mcls = class_resolve_lc(clc);
            if (!mcls)
                vm_raise(vm, ex_class("Error"), NULL, "Class \"%.*s\" not found",
                         (int)cl, s);
            tphp_class_ensure_flat(mcls);
            TMethod *m = tphp_class_find_method(mcls, mlc);
            if (!m || (!m->fn && !m->native) || m->is_abstract)
                vm_raise(vm, ex_class("Error"), NULL,
                         "Call to undefined method %s::%s()",
                         tstr_val(mcls->name), sep + 2);
            if (m->fn) fn = m->fn;
            else { nat = (NativeMethodFn)m->native; nat_self = NULL; }
        } else {
            bool isb;
            void *hit = resolve_fn(vm, (TStr *)callable.u.p, &isb);
            if (!hit)
                vm_raise(vm, ex_class("Error"), NULL,
                         "Call to undefined function %s()",
                         tstr_val((TStr *)callable.u.p));
            if (isb) bfn = (BuiltinFn)hit; else fn = (TFunc *)hit;
        }
    } else if (callable.t == T_ARR) {
        /* P2-B: array callable [$obj,'m'] / ['Cls','m'] */
        TArr *a = (TArr *)callable.u.p;
        if (a->n != 2)
            vm_raise(vm, ex_class("Error"), NULL,
                     "Callable array must have exactly 2 elements");
        uint32_t i0 = 0;
        while (i0 < a->dused && a->data[i0].t == T_UNDEF) i0++;
        uint32_t i1 = i0 + 1;
        while (i1 < a->dused && a->data[i1].t == T_UNDEF) i1++;
        if (i0 >= a->dused || i1 >= a->dused)
            vm_raise(vm, ex_class("Error"), NULL,
                     "Callable array must have exactly 2 elements");
        TVal *e0 = &a->data[i0], *e1 = &a->data[i1];
        if (e1->t != T_STR)
            vm_raise(vm, ex_class("TypeError"), NULL,
                     "Method name must be a string");
        TStr *mname = (TStr *)e1->u.p;
        char mlc[128];
        { const char *s = tstr_val(mname);
          size_t k = 0; for (; s[k] && k < sizeof mlc - 1; k++)
              mlc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
          mlc[k] = 0; }
        if (e0->t == T_OBJ) {
            recv = (TObj *)e0->u.p;
            mcls = recv->cls;
        } else if (e0->t == T_STR) {
            char clc[256];
            { const char *s = tstr_val((TStr *)e0->u.p);
              size_t k = 0; for (; s[k] && k < sizeof clc - 1; k++)
                  clc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
              clc[k] = 0; }
            extern TClass *class_resolve_lc(const char *fqcn_lc);
            mcls = class_resolve_lc(clc);
            if (!mcls)
                vm_raise(vm, ex_class("Error"), NULL, "Class \"%s\" not found",
                         tstr_val((TStr *)e0->u.p));
        } else {
            vm_raise(vm, ex_class("TypeError"), NULL,
                     "Callable array element #1 must be an object or a class name");
        }
        tphp_class_ensure_flat(mcls);
        TMethod *m = tphp_class_find_method(mcls, mlc);
        if (!m || (!m->fn && !m->native) || m->is_abstract)
            vm_raise(vm, ex_class("Error"), NULL,
                     "Call to undefined method %s::%s()",
                     tstr_val(mcls->name), tstr_val(mname));
        if (m->fn) fn = m->fn;
        else { nat = (NativeMethodFn)m->native; nat_self = recv; }
    } else if (callable.t == T_OBJ) {
        /* PHP: 带 __invoke 的对象是 callable（$obj(...) 与 call_user_func
         * 走同一条分派，避免"直接调用能跑、is_callable 说不行"的分裂） */
        TObj *o = (TObj *)callable.u.p;
        if (o->h.kind == GC_KIND_EX || !o->cls)
            vm_raise(vm, ex_class("TypeError"), NULL, "Value is not callable");
        tphp_class_ensure_flat(o->cls);
        TMethod *m = tphp_class_find_method(o->cls, "__invoke");
        if (!m || (!m->fn && !m->native) || m->is_abstract || m->is_static)
            vm_raise(vm, ex_class("TypeError"), NULL, "Value is not callable");
        recv = o;
        mcls = o->cls;
        if (m->fn) fn = m->fn;
        else { nat = (NativeMethodFn)m->native; nat_self = recv; }
    } else {
        vm_raise(vm, ex_class("TypeError"), NULL,
                 "Value is not callable");
    }
    if (bfn) {                       /* first-class builtin wrapper */
        return bfn(vm, args, n);
    }
    if (nat) {                       /* native (internal-class) method */
        return nat(vm, nat_self, args, n);
    }
    if (!fn)
        vm_raise(vm, ex_class("Error"), NULL, "Value is not callable");
    if (recv) {
        /* method frame: $this lives in slot 0 (owned ref — the closure's /
         * array's reference is separate), RECV moves params, extras die */
        if (vm->x.n_args + n > VM_MAX_ARGS)
            vm_raise(vm, ex_class("Error"), NULL, "Too many arguments");
        uint32_t base = (uint32_t)vm->x.n_args;
        int nq = (fn->is_variadic || n < fn->n_params) ? n : fn->n_params;
        for (int i = 0; i < n; i++) {
            vm->x.args[base + (uint32_t)i] = args[i];
            tval_addref(args[i]);
        }
        vm->x.n_args = (int)base + nq;
        for (int i = nq; i < n; i++)      /* extras: nobody consumes them */
            if (vm->x.args[base + (uint32_t)i].t != T_UNDEF) {
                tval_release(&vm->x.args[base + (uint32_t)i]);
                vm->x.args[base + (uint32_t)i] = (TVal){ .u.i = 0, .t = T_UNDEF };
            }
        VMCallRec *rec = call_push(vm, &g_magic_stub);
        rec->is_thunk = true;
        rec->uf = fn;
        rec->args_base = base;
        VMFrame *f = tm_malloc(sizeof *f);
        memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->vis_scope = NULL;
        f->fn = fn;
        f->file = fn->file;
        f->pc = 0;
        f->prev = vm->x.frame;
        f->args_base = base;
        f->n_args_sent = (uint16_t)nq;
        f->n_calls_at_entry = vm->x.n_calls;
        f->gen = NULL;
        uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
        f->slots = tm_malloc(sizeof(TVal) * nslots);
        for (uint32_t i = 0; i < nslots; i++)
            f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
        f->slots[0] = TV_OBJ(recv);
        tval_addref(TV_OBJ(recv));
        f->called_cls = mcls ? mcls : recv->cls;
        vm->x.n_args = (int)base;
        vm->x.frame = f;
        while (vm->x.frame == f) {
            TOp *cop = &f->fn->ops[f->pc];
            int32_t d = ((OpHandler)cop->handler)(vm, cop);
            if (d) f->pc += (uint32_t)d;
        }
        if (vm->x.exception) vm_unwind_jump(vm);
        return vm->x.thunk_ret;
    }
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
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->vis_scope = NULL;
    f->fn = fn;
    f->file = fn->file;
    f->pc = 0;
    f->prev = vm->x.frame;
    f->args_base = base;
    f->n_args_sent = (uint16_t)nq;
    /* P1-3: 闭包作用域——bindTo/bind 重绑过的用新 scope，否则继承声明类
     * （与 op_do_fcall 主路径的 frame_called_cls 一致；此前硬编码 NULL，
     * 经 usort/call() 等调用的闭包没有任何作用域） */
    f->called_cls = (clo && clo->scope) ? clo->scope
                  : (clo ? (TClass *)clo->fn->decl_cls : NULL);
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
    if (vm->x.exception) vm_unwind_jump(vm);
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
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->vis_scope = NULL;
    f->fn = fn;
    f->file = fn->file;
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
    tphp_run_shutdown_dtors();     /* P0-2: worker/闭包入口同样收尾析构 */
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
    memset(f, 0, sizeof *f);   /* zero gen/… — new fields default safely */
    f->vis_scope = NULL;
    f->fn = prog->main;
    f->file = prog->file;
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
    tphp_run_shutdown_dtors();     /* P0-2: 收尾析构（在 raise 钩子拆除前） */
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
