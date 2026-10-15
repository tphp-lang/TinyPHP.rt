/*
 * TinyPHP.rt - a small PHP runtime
 * opcode.h - opcode list, TOp layout (CALL dispatch, 32 bytes), TFunc
 */
#ifndef TPHP_OPCODE_H
#define TPHP_OPCODE_H

#include <setjmp.h>
#include "value.h"

/* operand kinds */
enum { OPD_UNUSED = 0, OPD_CONST, OPD_CV, OPD_TMP };

/* internal value type for undefined slots (defined in value.h) */

typedef enum {
    OP_CONST, OP_COPY,
    /* arithmetic / bitwise */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_CONCAT, OP_BW_AND, OP_BW_OR, OP_BW_XOR, OP_SHL, OP_SHR,
    /* comparison */
    OP_SEQ, OP_SNE, OP_EQ, OP_NE, OP_CMP, OP_LT, OP_LE, OP_BOOL_NOT,
    /* control flow */
    OP_JMP, OP_JMPZ, OP_JMPNZ,
    /* fetch */
    OP_FETCH_DIM_R, OP_FETCH_DIM_IS, OP_FETCH_GLOBAL,
    /* assignment */
    OP_ASSIGN, OP_ASSIGN_DIM, OP_ASSIGN_GLOBAL,
    /* calls */
    OP_INIT_FCALL, OP_INIT_DYN, OP_SEND_VAL, OP_SEND_VAR, OP_RECV,
    OP_DO_FCALL, OP_RETURN,
    /* construction */
    OP_ARRAY_INIT, OP_ARRAY_APPEND, OP_ARRAY_SPREAD,
    /* iteration */
    OP_FE_RESET, OP_FE_FETCH,
    /* misc */
    OP_ISSET_DIM, OP_ISSET_CV, OP_INC, OP_DEC,
    OP_ISSET_GLOBAL, OP_UNSET_GLOBAL, OP_UNSET_CV, OP_UNSET_DIM,
    OP_BOOL_VAL, OP_CAST, OP_NEG, OP_POS, OP_BIT_NOT,
    OP_JMP_NULL, OP_DEFINE_CONST, OP_FETCH_CONST,
    /* M2: closures, variadics, exceptions, first-class */
    OP_SEND_CAP, OP_CLOSURE_INIT, OP_GET_FUNC,
    OP_SEND_SPREAD, OP_RECV_VARIADIC,
    OP_NEW_EX, OP_THROW, OP_BIND_EX, OP_RETHROW, OP_FETCH_PROP,
    OP_FCC_DYN,   /* first-class callable from a value: closure passes through,
                     string fn name converts (M2) */
    /* M3: objects */
    OP_INIT_MCALL, OP_NEW_USER, OP_ASSIGN_PROP, OP_ISSET_PROP,
    OP_INIT_SMCALL, OP_FETCH_CCONST, OP_INSTANCEOF, OP_GET_CLASS,
    OP_ASSIGN_SPROP,   /* static property write: a1=kind, a2=lit key, tr=val */
    OP_UNSET_PROP,     /* unset($obj->prop): slot -> UNDEF, else __unset */
    OP_CLONE,          /* clone $obj: shallow copy + __clone hook */
    OP_FCC_METHOD,     /* P2-A $obj->m(...): bound method closure
                          (o1 = receiver, ar = method-name lit) */
    OP_FETCH_OSPROP,   /* $obj::$prop read: o1=obj, a2=lit "$name" */
    OP_ASSIGN_OSPROP,  /* $obj::$prop = v: o1=obj, a2=lit "$name", tr=val */
    OP_MAX,
} Opcode;

/* CALL-dispatch opcode: handler pointer embedded, 40 bytes.
 * Jump target convention: OP_JMP/JMPZ/JMPNZ/JMP_NULL keep the condition in
 * o1 and the jump target in `ar` (patch_jump writes ar). `ext` carries
 * auxiliary payloads (INC/DEC prefix flag, CLOSURE_INIT flags). `ic` is the
 * per-call-site inline cache (INIT_FCALL/INIT_DYN resolution memo). */
typedef struct TOp {
    void   *handler;   /* OpHandler */
    void   *ic;        /* inline cache (user TFunc* or builtin fn|1) */
    uint32_t a1, a2, ar;    /* operand payloads */
    uint16_t line;          /* PHP source line */
    uint16_t ext;           /* auxiliary (INC/DEC prefix flag, ...) */
    uint8_t  op;            /* Opcode */
    uint8_t  t1, t2, tr;    /* operand kinds */
} TOp;

/* per-function exception table (try regions; handler = catch chain entry) */
typedef struct TExEntry {
    uint32_t start_pc, end_pc, handler_pc;
    const uint16_t *catch_cls;   /* indexes into tphp_ex_classes[] (builtin
                                    fast path; entry i valid when catch_nm[i]
                                    is NULL) */
    const TStr **catch_nm;       /* P1-2: per-catch resolved class NAME (user
                                    exception classes and namespaced builtins);
                                    matching walks names across both worlds */
    uint8_t n_catches;
    uint8_t has_var;             /* 0 = catch without variable binding */
    uint16_t catch_slot;         /* CV slot receiving the exception object */
} TExEntry;

typedef struct TFunc {
    TStr *name;
    uint16_t n_params, n_locals, n_tmps; /* slots: [0..n_params) args, then locals, then tmps */
    TStr  **local_names;   /* n_locals entries (first n_params are parameter names) */
    TStr  **param_types;   /* n_params entries; NULL = untyped (P1-1) */
    TStr   *ret_type;      /* declared return type; NULL = none (P1-1) */
    TVal   *defaults;      /* n_params entries; T_UNDEF marker = required */
    uint16_t *default_thunk; /* per-param thunk TFunc index (0 = none) */
    bool    *is_global;    /* slot is a `global` alias */
    uint32_t n_ops; TOp *ops;
    uint32_t n_lits; TVal *lits;
    struct TExEntry *ex; uint16_t n_ex;   /* exception table */
    uint16_t n_cap;        /* closure capture slots at frame start */
    bool is_main;
    bool is_closure;
    bool is_method;        /* M3: slot 0 (after captures) holds $this */
    bool is_variadic;      /* last param is ...$x: extras go to RECV_VARIADIC */
    void *decl_cls;        /* TClass*: class this method was declared in */
} TFunc;

typedef struct Program {
    const char *file;    /* source path (fatal messages) */
    TFunc *main;
    TFunc **funcs;
    int n_funcs;
} Program;

/* ---- interpreter context (shared with builtin.c / compile.c) ---- */
typedef struct VM VM;

typedef TVal (*BuiltinFn)(VM *vm, TVal *args, int n);
/* native method on an internal class: receives the receiver object */
typedef TVal (*NativeMethodFn)(VM *vm, void *self, TVal *args, int n);
typedef struct { const char *name; BuiltinFn fn; } Builtin;
extern const Builtin tphp_builtins[];
extern const int tphp_builtin_count;

/* raise a typed exception from anywhere holding a VM* */
void tphp_vm_raise(VM *vm, const char *cls, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* evaluate a zero-arg compiled thunk (runtime default values) */
TVal tphp_call_thunk(VM *vm, uint16_t fn_idx);

#define VM_MAX_CALLS 256
#define VM_MAX_ARGS  4096

typedef struct VMFrame {
    TFunc *fn;
    uint32_t pc;
    TVal *slots;
    struct VMFrame *prev;
    uint32_t args_base;
    uint16_t n_args_sent;
    void *called_cls;    /* TClass*: actual class for static:: (late binding) */
    void *vis_scope;     /* P3: 可见性作用域覆盖（parent::__construct 帧 =
                            声明类）；NULL = 用 called_cls */
    uint32_t n_calls_at_entry;   /* n_calls when this frame took over; unwind
                                    restores it (M5-b: leftover call recs of
                                    builtin/magic raises) */
} VMFrame;

typedef struct VMCallRec {
    bool is_builtin;
    bool is_thunk;
    bool is_method;      /* method call: this_obj carries the receiver */
    bool is_ctor;        /* NEW_USER frame: RETURN writes $this to dst */
    bool scope_is_called;/* P3: parent::__construct 帧——作用域取
                            called_cls（声明类）而非接收者类 */
    bool is_enum_builtin;/* INIT_SMCALL on enum ::from/tryFrom/cases */
    bool is_ex_getter;   /* INIT_MCALL on an exception object: getter_val ready */
    TFunc *uf;
    TClosure *clo;
    BuiltinFn bf;
    void *nm_fn;         /* NativeMethodFn when calling an internal class
                            method (is_builtin is set for the DO_FCALL path) */
    void *ffi_fn;        /* M5.5: FFIFunc* when calling through an FFI object
                            ($ffi->symbol(...); is_builtin is set) */
    void *this_obj;      /* TObj* receiver when is_method */
    void *called_cls;    /* TClass*: static-call resolution for static:: */
    TVal getter_val;     /* precomputed exception-getter result */
    uint8_t magic_kind;  /* 0 none / 1 __call / 2 __callStatic (deferred to
                            DO_FCALL where all args are SENDed) */
    TStr *magic_name;    /* member name stamped by INIT for magic dispatch */
    uint32_t args_base;
    uint16_t n_args;
    uint8_t dst_kind;
    uint32_t dst_slot;
    int line;
} VMCallRec;

/* per-coroutine execution context (M4): EVERYTHING that defines a paused
 * coroutine — frame chain, call stack, argument area, the longjmp landing
 * point and the pending exception. The VM holds the live copy of exactly one
 * of these; switching coroutines exchanges the whole struct (a flat ~1.5KB
 * copy, O(1)). Big arrays are heap-allocated lazily per context — a
 * suspended coroutine owns its own stack slice. */
typedef struct TExecCtx {
    VMFrame *frame;        /* NULL when idle (no frame running) */
    VMCallRec *calls;      /* VM_MAX_CALLS, lazily allocated */
    int n_calls;
    TVal *args;            /* VM_MAX_ARGS, lazily allocated */
    int n_args;
    TVal *caps;            /* 256, lazily allocated */
    int n_caps;
    struct { const char *fn; int line; } trace[64];
    int n_trace;
    TVal thunk_ret;        /* expression thunk result (per-coroutine) */
    jmp_buf unwind;        /* exception landing point (set per coroutine) */
    TExObj *exception;     /* exception currently unwinding */
    bool done;             /* current coroutine finished */
    bool uncaught;         /* ...with an exception nobody caught */
    TVal *coro_result;     /* non-NULL: coroutine root — bottom RETURN moves
                              the value here instead of discarding it */
} TExecCtx;

/* lazy allocation of the per-context heap arrays (vm.c) */
void tphp_ctx_ensure(TExecCtx *x);

struct VM {
    TExecCtx x;            /* execution state of the RUNNING coroutine */
    VMFrame *main_frame;   /* shared global-variable frame (script scope) */
    Program *prog;
    int exit_code;
    struct { TStr *name; TVal v; } consts[256];
    int n_consts;
};



/* handler signature: returns pc delta (>0 next, 0 = handler manages pc/frame) */
typedef int32_t (*OpHandler)(void *vm, TOp *op);
extern const OpHandler tphp_op_handlers[OP_MAX];

const char *tphp_op_name(uint8_t op);

#endif /* TPHP_OPCODE_H */
