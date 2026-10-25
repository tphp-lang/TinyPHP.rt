/*
 * TinyPHP.rt - a small PHP runtime
 * jit.c - IR baseline JIT (PLAN/ir-jit.md v1)
 *
 * Closed scalar world: eligible functions only touch int/float/bool/null.
 * Frame slots stay the single source of truth; IR loads/stores them and
 * lets IR's optimizer clean within-block redundancy. Slow paths call the
 * tval_* family (one semantic source).
 *
 * JitEntry ABI (PLAN/ir-jit.md §3):
 *   int fn(VM *vm, TVal *slots, TVal *args_in, TVal *ret);
 *   1 = *ret owned by caller; 0 = deopt (args_in/slots untouched).
 */
#include "value.h"
#include "opcode.h"
#include "tphp.h"
#include "jit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void tphp_vm_frame_destroy(VMFrame *f);   /* vm.c */

#ifdef TPHP_JIT

#include "ir.h"
#include "ir_builder.h"
#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_jit_mtx;
#else
#include <pthread.h>
static pthread_mutex_t g_jit_mtx = PTHREAD_MUTEX_INITIALIZER;
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define JIT_PLAT_OK 0
#else
#define JIT_PLAT_OK 1
#endif

static bool g_jit_on;

static int jit_dbg(void) {
    static int c;
    if (!c) { const char *e = getenv("TPHP_JIT_DEBUG"); c = e && e[0] ? 2 : 1; }
    return c == 2;
}

static void jit_lock(void) {
#ifdef _WIN32
    EnterCriticalSection(&g_jit_mtx);
#else
    pthread_mutex_lock(&g_jit_mtx);
#endif
}
static void jit_unlock(void) {
#ifdef _WIN32
    LeaveCriticalSection(&g_jit_mtx);
#else
    pthread_mutex_unlock(&g_jit_mtx);
#endif
}

bool tphp_jit_available(void) { return g_jit_on; }

void tphp_jit_init(void) {
#ifdef _WIN32
    static bool inited;
    if (!inited) { InitializeCriticalSection(&g_jit_mtx); inited = true; }
#endif
    const char *e = getenv("TPHP_JIT");
    g_jit_on = JIT_PLAT_OK && !(e && e[0] == '0' && e[1] == '\0');
}

_Static_assert(offsetof(TVal, u) == 0, "TVal.u offset");
_Static_assert(offsetof(TVal, t) == 8, "TVal.t offset");
_Static_assert(sizeof(TVal) == 16, "TVal size");
#define TVAL_SZ  16
#define TVAL_TAG 8

#define JT_NULL  ((int32_t)T_NULL)
#define JT_FALSE ((int32_t)T_FALSE)
#define JT_TRUE  ((int32_t)T_TRUE)
#define JT_INT   ((int32_t)T_INT)
#define JT_FLOAT ((int32_t)T_FLOAT)

/* ---- slow helpers ---- */
void tphp_jit_binop_slow(VM *vm, int32_t opi, TVal *dst,
                         int32_t ta, int64_t pa, int32_t tb, int64_t pb) {
    (void)vm;
    TVal a = { .u.i = pa, .t = (TType)ta };
    TVal b = { .u.i = pb, .t = (TType)tb };
    TVal r;
    switch (opi) {
    case OP_ADD:  r = tval_add(a, b); break;
    case OP_SUB:  r = tval_sub(a, b); break;
    case OP_MUL:  r = tval_mul(a, b); break;
    case OP_DIV:  r = tval_div(a, b); break;
    case OP_MOD:  r = tval_mod(a, b); break;
    case OP_POW:  r = tval_pow(a, b); break;
    case OP_BW_AND: r = tval_band(a, b); break;
    case OP_BW_OR:  r = tval_bor(a, b); break;
    case OP_BW_XOR: r = tval_bxor(a, b); break;
    case OP_SHL:  r = tval_shl(a, b); break;
    case OP_SHR:  r = tval_shr(a, b); break;
    case OP_NEG:  r = tval_neg(a); break;
    case OP_POS:  r = tval_pos(a); break;
    case OP_BOOL_NOT: r = tval_bool_not(a); break;
    default:      r = TV_NULL; break;
    }
    tval_release(dst);
    *dst = r;
}

void tphp_jit_cmp_slow(VM *vm, int32_t opi, TVal *dst,
                       int32_t ta, int64_t pa, int32_t tb, int64_t pb) {
    (void)vm;
    TVal a = { .u.i = pa, .t = (TType)ta };
    TVal b = { .u.i = pb, .t = (TType)tb };
    TVal r;
    switch (opi) {
    case OP_EQ:  r = tval_loose_eq(a, b) ? TV_TRUE : TV_FALSE; break;
    case OP_NE:  r = tval_loose_eq(a, b) ? TV_FALSE : TV_TRUE; break;
    case OP_SEQ: r = tval_strict_eq(a, b) ? TV_TRUE : TV_FALSE; break;
    case OP_SNE: r = tval_strict_eq(a, b) ? TV_FALSE : TV_TRUE; break;
    case OP_LT:  r = tval_compare(a, b) < 0 ? TV_TRUE : TV_FALSE; break;
    case OP_LE:  r = tval_compare(a, b) <= 0 ? TV_TRUE : TV_FALSE; break;
    case OP_CMP: r = TV_INT(tval_compare(a, b)); break;
    default:     r = TV_NULL; break;
    }
    tval_release(dst);
    *dst = r;
}

/* JIT->JIT call trampoline (section 8): TLS slot stack - closed-world
 * callees never raise and never touch vm->x.frame. Recursion safe via
 * bump pointer (fib depth ~28 x ~10 slots fits easily). */
int tphp_jit_call(VM *vm, TFunc *fn, TVal *args_in, TVal *ret) {
    JitEntry e = (JitEntry)fn->jit_entry;
    if (!e) return 0;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    enum { JSIZE = 2048 };
    static _Thread_local TVal g_jstack[JSIZE];
    static _Thread_local uint32_t g_jsp;
    if (g_jsp + nslots > JSIZE) return 0;
    TVal *slots = &g_jstack[g_jsp];
    g_jsp += nslots;
    for (uint32_t i = 0; i < nslots; i++)
        slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    int ok = e(vm, slots, args_in, ret);
    g_jsp -= nslots;
    return ok;
}
/* ================================================================
 * Eligibility (PLAN/ir-jit.md §5)
 * ================================================================ */
static bool jit_eligible_inner(TFunc *fn, int depth);

static bool lit_scalar(TVal v) {
    return v.t == T_INT || v.t == T_FLOAT || v.t == T_TRUE ||
           v.t == T_FALSE || v.t == T_NULL;
}

static bool jit_eligible_inner(TFunc *fn, int depth) {
    if (depth > 16) return false;
    if (!fn || fn->is_main || fn->is_closure || fn->is_method ||
        fn->is_variadic || fn->is_generator || fn->n_ex > 0 ||
        fn->n_cap > 0 || fn->n_params > 8)
        return false;
    if (fn->param_types) {
        for (uint16_t i = 0; i < fn->n_params; i++)
            if (fn->param_types[i]) return false;
    }
    if (fn->defaults) {
        for (uint16_t i = 0; i < fn->n_params; i++)
            if (fn->defaults[i].t != T_UNDEF) return false;
    }
    if (fn->default_thunk) {
        for (uint16_t i = 0; i < fn->n_params; i++)
            if (fn->default_thunk[i]) return false;
    }
    if (fn->is_global) {
        for (uint16_t i = 0; i < fn->n_locals; i++)
            if (fn->is_global[i]) return false;
    }
    if (fn->ret_type) {
        const char *rt = tstr_val(fn->ret_type);
        if (!(strcmp(rt, "int") == 0 || strcmp(rt, "float") == 0 ||
              strcmp(rt, "bool") == 0 || strcmp(rt, "mixed") == 0))
            return false;
    }

    uint32_t recv_seen = 0;
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        switch (op->op) {
        case OP_RECV:
            if (i != recv_seen) return false;   /* leading block only */
            recv_seen++;
            if (recv_seen > fn->n_params) return false;
            break;
        case OP_CONST:
            if (!lit_scalar(fn->lits[op->a1])) return false;
            break;
        case OP_COPY: case OP_ASSIGN: case OP_INC: case OP_DEC:
        case OP_NEG: case OP_POS: case OP_BOOL_NOT: case OP_BOOL_VAL:
            break;
        case OP_CAST: {
            if (op->t2 != OPD_CONST) return false;
            int64_t ct = fn->lits[op->a2].u.i;
            if (ct != 1 && ct != 2 && ct != 3) return false;  /* int/bool/float */
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_BW_AND: case OP_BW_OR: case OP_BW_XOR:
        case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE:
        case OP_LT: case OP_LE: case OP_CMP:
            break;
        case OP_DIV: case OP_MOD: {
            if (op->t2 != OPD_CONST) return false;
            TVal d = fn->lits[op->a2];
            if (d.t == T_INT ? d.u.i == 0 : !(d.t == T_FLOAT && d.u.d != 0.0))
                return false;
            break;
        }
        case OP_SHL: case OP_SHR:
            if (op->t2 != OPD_CONST || fn->lits[op->a2].t != T_INT)
                return false;
            break;
        case OP_JMP: case OP_JMPZ: case OP_JMPNZ:
        case OP_RETURN:
            break;
        case OP_INIT_FCALL: {
            TFunc *callee = (TFunc *)op->ic;
            if (!op->ic || ((uintptr_t)op->ic & 1)) return false;
            if (callee != fn && !jit_eligible_inner(callee, depth + 1))
                return false;
            break;
        }
        case OP_SEND_VAL: case OP_SEND_VAR:
        case OP_DO_FCALL:
            break;
        default:
            return false;
        }
    }
    if (recv_seen != fn->n_params) return false;
    return true;
}

bool tphp_jit_eligible(TFunc *fn) {
    bool ok = jit_eligible_inner(fn, 0);
    static int dbg;
    if (!dbg) { const char *e = getenv("TPHP_JIT_DEBUG"); dbg = e && e[0] ? 2 : 1; }
    if (dbg == 2 && fn && fn->name)
        fprintf(stderr, "[jit] eligible(%s)=%d\n", tstr_val(fn->name), ok);
    return ok;
}

/* ================================================================
 * Lowering
 * ================================================================ */
typedef struct {
    ir_ctx *ctx;
    TFunc *fn;
    ir_ref r_vm, r_slots, r_args, r_ret;
    int in_loop;
    uint32_t loop_header;
    ir_ref loop_ref;
    int in_loop_cond;
    int fail;
    /* folded call in progress (INIT_FCALL … DO_FCALL, args evaluated in between) */
    TFunc *p_callee;
    uint32_t p_argc, p_arg_slot[9];
} JitC;

/* ir_builder macros expand to `ctx` — every lowering fn binds it locally */
#define JIT_CTX() ir_ctx *ctx = j->ctx; (void)ctx

static ir_ref slot_addr(JitC *j, uint32_t idx) {
    JIT_CTX();
    return ir_ADD_A(j->r_slots,
                    ir_CONST_ADDR((uintptr_t)idx * TVAL_SZ));
}
static ir_ref load_tag(JitC *j, ir_ref addr) {
    JIT_CTX();
    return ir_LOAD_I32(ir_ADD_A(addr, ir_CONST_ADDR(TVAL_TAG)));
}
static ir_ref load_pay(JitC *j, ir_ref addr) {
    JIT_CTX();
    return ir_LOAD_I64(addr);
}
static void store_pay(JitC *j, ir_ref addr, ir_ref pay) {
    JIT_CTX();
    ir_STORE(addr, pay);
}
static void store_tag(JitC *j, ir_ref addr, ir_ref tag) {
    JIT_CTX();
    ir_STORE(ir_ADD_A(addr, ir_CONST_ADDR(TVAL_TAG)), tag);
}
static void store_imm(JitC *j, ir_ref addr, int32_t tag, ir_ref pay) {
    JIT_CTX();
    store_pay(j, addr, pay);
    store_tag(j, addr, ir_CONST_I32(tag));
}

static ir_ref opnd_tag(JitC *j, uint8_t kind, uint32_t a) {
    JIT_CTX();
    if (kind == OPD_CONST) return ir_CONST_I32((int32_t)j->fn->lits[a].t);
    return load_tag(j, slot_addr(j, a));
}
static ir_ref opnd_pay(JitC *j, uint8_t kind, uint32_t a) {
    JIT_CTX();
    if (kind == OPD_CONST) return ir_CONST_I64(j->fn->lits[a].u.i);
    return load_pay(j, slot_addr(j, a));
}

static void call_slow_binop(JitC *j, int opi, ir_ref da,
                            ir_ref ta, ir_ref pa, ir_ref tb, ir_ref pb) {
    JIT_CTX();
    ir_ref args[7] = { j->r_vm, ir_CONST_I32(opi), da, ta, pa, tb, pb };
    ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_binop_slow), 7, args);
}
static void call_slow_cmp(JitC *j, int opi, ir_ref da,
                          ir_ref ta, ir_ref pa, ir_ref tb, ir_ref pb) {
    JIT_CTX();
    ir_ref args[7] = { j->r_vm, ir_CONST_I32(opi), da, ta, pa, tb, pb };
    ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_cmp_slow), 7, args);
}

static ir_ref make_truthy(JitC *j, ir_ref tag, ir_ref pay, ir_ref addr) {
    JIT_CTX();
    ir_ref is_null = ir_EQ(tag, ir_CONST_I32(JT_NULL));
    ir_ref is_false = ir_EQ(tag, ir_CONST_I32(JT_FALSE));
    ir_ref is_int = ir_EQ(tag, ir_CONST_I32(JT_INT));
    ir_ref int0 = ir_AND(IR_BOOL, is_int, ir_EQ(pay, ir_CONST_I64(0)));
    ir_ref falsy = ir_OR(IR_BOOL, ir_OR(IR_BOOL, is_null, is_false), int0);
    ir_ref is_flt = ir_EQ(tag, ir_CONST_I32(JT_FLOAT));
    ir_ref d = ir_LOAD_D(addr);
    ir_ref flt0 = ir_AND(IR_BOOL, is_flt, ir_EQ(d, ir_CONST_DOUBLE(0.0)));
    ir_ref falsy2 = ir_OR(IR_BOOL, falsy, flt0);
    return ir_NOT_B(falsy2);
}
/* Diamond: both sides live → END both + MERGE (IR hard rule) */
static void store_bool(JitC *j, ir_ref da, ir_ref cond) {
    JIT_CTX();
    ir_ref ifc = ir_IF(cond);
    ir_IF_TRUE(ifc);
        store_imm(j, da, JT_TRUE, ir_CONST_I64(0));
        ir_ref eT = ir_END();
    ir_IF_FALSE(ifc);
        store_imm(j, da, JT_FALSE, ir_CONST_I64(0));
        ir_ref eF = ir_END();
    ir_MERGE_2(eT, eF);
}

static void lower_binop(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = slot_addr(j, op->ar);
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    ir_ref tb, pb;
    if (op->t2 == OPD_CONST) {
        tb = ir_CONST_I32((int32_t)j->fn->lits[op->a2].t);
        pb = ir_CONST_I64(j->fn->lits[op->a2].u.i);
    } else {
        ir_ref ba = slot_addr(j, op->a2);
        tb = load_tag(j, ba); pb = load_pay(j, ba);
    }
    if (op->op == OP_ADD || op->op == OP_SUB) {
        ir_ref both = ir_AND(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_INT)),
                             ir_EQ(tb, ir_CONST_I32(JT_INT)));
        ir_ref ifx = ir_IF(both);
        ir_IF_TRUE(ifx);
            ir_ref sum = op->op == OP_ADD ? ir_ADD_I64(pa, pb)
                                          : ir_SUB_I64(pa, pb);
            ir_ref of;
            if (op->op == OP_ADD)
                of = ir_LT(ir_AND_I64(ir_XOR_I64(pa, sum),
                                      ir_XOR_I64(pb, sum)),
                           ir_CONST_I64(0));
            else
                of = ir_LT(ir_AND_I64(ir_XOR_I64(pa, pb),
                                      ir_XOR_I64(pa, sum)),
                           ir_CONST_I64(0));
            ir_ref ifof = ir_IF(of);
            ir_IF_TRUE(ifof);
                ir_ref fd = ir_ADD_D(ir_BITCAST_D(pa), ir_BITCAST_D(pb));
                store_imm(j, da, JT_FLOAT, ir_BITCAST_U64(fd));
                ir_ref eOvf = ir_END();
            ir_IF_FALSE(ifof);
                store_imm(j, da, JT_INT, sum);
                ir_ref eInt = ir_END();
            ir_MERGE_2(eOvf, eInt);
            ir_ref eFast = ir_END();
        ir_IF_FALSE(ifx);
            call_slow_binop(j, op->op, da, ta, pa, tb, pb);
            ir_ref eSlow = ir_END();
        ir_MERGE_2(eFast, eSlow);
    } else {
        call_slow_binop(j, op->op, da, ta, pa, tb, pb);
    }
}

static void lower_cmp(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = slot_addr(j, op->ar);
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    ir_ref tb, pb;
    if (op->t2 == OPD_CONST) {
        tb = ir_CONST_I32((int32_t)j->fn->lits[op->a2].t);
        pb = ir_CONST_I64(j->fn->lits[op->a2].u.i);
    } else {
        ir_ref ba = slot_addr(j, op->a2);
        tb = load_tag(j, ba); pb = load_pay(j, ba);
    }
    ir_ref both_int = ir_AND(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_INT)),
                             ir_EQ(tb, ir_CONST_I32(JT_INT)));
    if (op->op == OP_CMP) {
        ir_ref ifx = ir_IF(both_int);
        ir_IF_TRUE(ifx);
            ir_ref lt = ir_LT(pa, pb);
            ir_ref gt = ir_LT(pb, pa);
            ir_ref iflt = ir_IF(lt);
            ir_IF_TRUE(iflt);
                store_imm(j, da, JT_INT, ir_CONST_I64(-1));
                ir_ref eNeg = ir_END();
            ir_IF_FALSE(iflt);
                ir_ref ifgt = ir_IF(gt);
                ir_IF_TRUE(ifgt);
                    store_imm(j, da, JT_INT, ir_CONST_I64(1));
                    ir_ref ePos = ir_END();
                ir_IF_FALSE(ifgt);
                    store_imm(j, da, JT_INT, ir_CONST_I64(0));
                    ir_ref eZ = ir_END();
                ir_MERGE_2(ePos, eZ);
                ir_ref eGe = ir_END();
            ir_MERGE_2(eNeg, eGe);
            ir_ref eInt = ir_END();
        ir_IF_FALSE(ifx);
            call_slow_cmp(j, op->op, da, ta, pa, tb, pb);
            ir_ref eSlow = ir_END();
        ir_MERGE_2(eInt, eSlow);
        return;
    }
    ir_ref ifx = ir_IF(both_int);
    ir_IF_TRUE(ifx);
        ir_ref c;
        switch (op->op) {
        case OP_EQ:  case OP_SEQ: c = ir_EQ(pa, pb); break;
        case OP_NE:  case OP_SNE: c = ir_NE(pa, pb); break;
        case OP_LT:  c = ir_LT(pa, pb); break;
        default:     c = ir_LE(pa, pb); break;
        }
        store_bool(j, da, c);
        ir_ref eFast = ir_END();
    ir_IF_FALSE(ifx);
        call_slow_cmp(j, op->op, da, ta, pa, tb, pb);
        ir_ref eSlow = ir_END();
    ir_MERGE_2(eFast, eSlow);
}

static void lower_un(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = slot_addr(j, op->ar);
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    switch (op->op) {
    case OP_COPY: case OP_ASSIGN:
        store_pay(j, da, pa); store_tag(j, da, ta); break;
    case OP_BOOL_NOT: {
        ir_ref t = make_truthy(j, ta, pa, aa);
        store_bool(j, da, ir_NOT_B(t));
        break;
    }
    case OP_BOOL_VAL: {
        ir_ref t = make_truthy(j, ta, pa, aa);
        store_bool(j, da, t);
        break;
    }
    case OP_NEG:
        call_slow_binop(j, OP_NEG, da, ta, pa, ta, pa);
        break;
    case OP_POS:
        call_slow_binop(j, OP_POS, da, ta, pa, ta, pa);
        break;
    case OP_CAST: {
        int64_t ct = j->fn->lits[op->a2].u.i;   /* 1=int 2=bool 3=float */
        if (ct == 2) {
            store_bool(j, da, make_truthy(j, ta, pa, aa));
        } else if (ct == 1) {
            ir_ref ifi = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_INT)));
            ir_IF_TRUE(ifi);
                store_imm(j, da, JT_INT, pa);
                ir_ref eI = ir_END();
            ir_IF_FALSE(ifi);
                ir_ref iff = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_FLOAT)));
                ir_IF_TRUE(iff);
                    store_imm(j, da, JT_INT, ir_FP2I64(ir_LOAD_D(aa)));
                    ir_ref eF = ir_END();
                ir_IF_FALSE(iff);
                    ir_ref ift = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_TRUE)));
                    ir_IF_TRUE(ift);
                        store_imm(j, da, JT_INT, ir_CONST_I64(1));
                        ir_ref eT = ir_END();
                    ir_IF_FALSE(ift);
                        store_imm(j, da, JT_INT, ir_CONST_I64(0));
                        ir_ref e0 = ir_END();
                    ir_MERGE_2(eT, e0);
                    ir_ref eRest = ir_END();
                ir_MERGE_2(eF, eRest);
                ir_ref eNF = ir_END();
            ir_MERGE_2(eI, eNF);
        } else {
            ir_ref ifl = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_FLOAT)));
            ir_IF_TRUE(ifl);
                store_imm(j, da, JT_FLOAT, ir_BITCAST_U64(ir_LOAD_D(aa)));
                ir_ref eF = ir_END();
            ir_IF_FALSE(ifl);
                ir_ref ifi = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_INT)));
                ir_IF_TRUE(ifi);
                    store_imm(j, da, JT_FLOAT, ir_BITCAST_U64(ir_INT2D(pa)));
                    ir_ref eI = ir_END();
                ir_IF_FALSE(ifi);
                    ir_ref ift = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_TRUE)));
                    ir_IF_TRUE(ift);
                        store_imm(j, da, JT_FLOAT,
                                  ir_BITCAST_U64(ir_CONST_DOUBLE(1.0)));
                        ir_ref eT = ir_END();
                    ir_IF_FALSE(ift);
                        store_imm(j, da, JT_FLOAT,
                                  ir_BITCAST_U64(ir_CONST_DOUBLE(0.0)));
                        ir_ref e0 = ir_END();
                    ir_MERGE_2(eT, e0);
                    ir_ref eRest = ir_END();
                ir_MERGE_2(eI, eRest);
                ir_ref eNF = ir_END();
            ir_MERGE_2(eF, eNF);
        }
        break;
    }
    default: break;
    }
}

static void lower_incdec(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = aa;
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    ir_ref isint = ir_EQ(ta, ir_CONST_I32(JT_INT));
    ir_ref d1 = op->op == OP_INC ? ir_CONST_I64(1) : ir_CONST_I64(-1);
    ir_ref ifx = ir_IF(isint);
    ir_IF_TRUE(ifx);
        ir_ref sum = ir_ADD_I64(pa, d1);
        ir_ref of = ir_LT(ir_AND_I64(ir_XOR_I64(pa, sum),
                                     ir_XOR_I64(pa, d1)),
                          ir_CONST_I64(0));
        ir_ref ifof = ir_IF(of);
        ir_IF_TRUE(ifof);
            double fd = op->op == OP_INC ? 1.0 : -1.0;
            ir_ref fdv = ir_ADD_D(ir_BITCAST_D(pa), ir_CONST_DOUBLE(fd));
            store_imm(j, da, JT_FLOAT, ir_BITCAST_U64(fdv));
            ir_ref eOvf = ir_END();
        ir_IF_FALSE(ifof);
            store_imm(j, da, JT_INT, sum);
            ir_ref eInt = ir_END();
        ir_MERGE_2(eOvf, eInt);
        ir_ref eFast = ir_END();
    ir_IF_FALSE(ifx);
        double fd2 = op->op == OP_INC ? 1.0 : -1.0;
        ir_ref args[7] = {
            j->r_vm, ir_CONST_I32(op->op == OP_INC ? OP_ADD : OP_SUB),
            da, ta, pa, ir_CONST_I32(JT_FLOAT),
            ir_BITCAST_U64(ir_CONST_DOUBLE(fd2))
        };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_binop_slow), 7, args);
        ir_ref eSlow = ir_END();
    ir_MERGE_2(eFast, eSlow);
}

/* finish a folded call at DO_FCALL (args already recorded in j->p_*) */
static void lower_call_finish(JitC *j, TOp *doop) {
    JIT_CTX();
    TFunc *callee = j->p_callee;
    uint32_t argc = j->p_argc;
    if (!callee || argc != callee->n_params) { j->fail = 1; return; }

    ir_ref args_arr = ir_ALLOCA(ir_CONST_I64((uintptr_t)argc * TVAL_SZ));
    for (uint32_t k = 0; k < argc; k++) {
        ir_ref sa = slot_addr(j, j->p_arg_slot[k]);
        ir_ref d = ir_ADD_A(args_arr, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
        store_pay(j, d, load_pay(j, sa));
        store_tag(j, d, load_tag(j, sa));
    }
    ir_ref ret_sc = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
    ir_ref args_t[4] = { j->r_vm, ir_CONST_ADDR((uintptr_t)callee),
                         args_arr, ret_sc };
    ir_ref ok = ir_CALL_N(IR_I32, ir_CONST_ADDR((uintptr_t)tphp_jit_call),
                          4, args_t);
    (void)ok;
    /* v1: callee prologue guards are unreachable (chained eligibility keeps
     * every arg in the closed INT world) — no deopt branch; load result. */
    if (doop->tr != OPD_UNUSED) {
        ir_ref da = slot_addr(j, doop->ar);
        store_pay(j, da, ir_LOAD_I64(ret_sc));
        store_tag(j, da, ir_LOAD_I32(ir_ADD_A(ret_sc,
                                              ir_CONST_ADDR(TVAL_TAG))));
    }
    j->p_callee = NULL;
}

static void lower_return(JitC *j, TOp *op) {
    JIT_CTX();
    if (op->t1 != OPD_UNUSED) {
        ir_ref ta = opnd_tag(j, op->t1, op->a1);
        ir_ref pa = opnd_pay(j, op->t1, op->a1);
        store_pay(j, j->r_ret, pa);
        store_tag(j, j->r_ret, ta);
    } else {
        store_imm(j, j->r_ret, JT_NULL, ir_CONST_I64(0));
    }
    ir_RETURN(ir_CONST_I32(1));
}

/* ---- structured emission ---- */
typedef enum { TR_END, TR_RET, TR_LOOPBACK, TR_FAIL } TrapKind;
typedef struct { TrapKind kind; ir_ref end; uint32_t target; } Trap;

static Trap emit_seq(JitC *j, uint32_t pc, const uint32_t *stops, int nstops);

static int find_join(TFunc *fn, uint32_t p, uint32_t T,
                     uint32_t *join_out, uint32_t *else_out) {
    int depth = 0;
    for (uint32_t q = p + 1; q < fn->n_ops; q++) {
        TOp *op = &fn->ops[q];
        switch (op->op) {
        case OP_JMPZ: case OP_JMPNZ:
            depth++;
            break;
        case OP_JMP:
            if (depth == 0) {
                if (op->ar <= q || op->ar < T) return 0;
                *join_out = op->ar; *else_out = T;
                return 1;
            }
            depth--;
            break;
        case OP_RETURN:
            /* terminal: fallthrough into T is impossible from here */
            break;
        default:
            /* fallthrough arrival at T closes an if-without-else */
            if (q + 1 == T && depth == 0) {
                *join_out = T; *else_out = 0; return 1;
            }
            break;
        }
    }
    return 0;
}

static int is_backedge_target(TFunc *fn, uint32_t pc) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if ((op->op == OP_JMP || op->op == OP_JMPZ || op->op == OP_JMPNZ) &&
            op->ar == pc && pc <= i)
            return 1;
    }
    return 0;
}

static Trap trap_end(ir_ref e, uint32_t t) { Trap x = { TR_END, e, t }; return x; }
static Trap trap_ret(void)  { Trap x = { TR_RET, 0, 0 }; return x; }
static Trap trap_fail(void) { Trap x = { TR_FAIL, 0, 0 }; return x; }
static Trap trap_lb(void)   { Trap x = { TR_LOOPBACK, 0, 0 }; return x; }

static int stop_has(const uint32_t *stops, int n, uint32_t pc) {
    for (int i = 0; i < n; i++) if (stops[i] == pc) return 1;
    return 0;
}

/* cond operand address (scratch for CONST) */
static ir_ref cond_addr(JitC *j, uint8_t kind, uint32_t a, ir_ref *tag_out,
                        ir_ref *pay_out) {
    JIT_CTX();
    if (kind == OPD_CONST) {
        ir_ref sc = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
        store_imm(j, sc, (int32_t)j->fn->lits[a].t,
                  ir_CONST_I64(j->fn->lits[a].u.i));
        *tag_out = load_tag(j, sc);
        *pay_out = load_pay(j, sc);
        return sc;
    }
    ir_ref ad = slot_addr(j, a);
    *tag_out = load_tag(j, ad);
    *pay_out = load_pay(j, ad);
    return ad;
}

static Trap emit_seq(JitC *j, uint32_t pc, const uint32_t *stops, int nstops) {
    JIT_CTX();
    if (j->fail) return trap_fail();
    for (;;) {
        if (pc >= j->fn->n_ops) return trap_fail();
        if (stop_has(stops, nstops, pc)) return trap_end(ir_END(), pc);

        if (!j->in_loop && is_backedge_target(j->fn, pc)) {
            ir_ref ein = ir_END();
            j->loop_ref = ir_LOOP_BEGIN(ein);
            j->in_loop = 1;
            j->loop_header = pc;
            j->in_loop_cond = 1;
        }

        TOp *op = &j->fn->ops[pc];
        switch (op->op) {
        case OP_CONST: {
            JIT_CTX();
            TVal v = j->fn->lits[op->a1];
            store_imm(j, slot_addr(j, op->ar), (int32_t)v.t,
                      ir_CONST_I64(v.u.i));
            pc++;
            break;
        }
        case OP_COPY: case OP_ASSIGN:
        case OP_BOOL_NOT: case OP_BOOL_VAL: case OP_NEG: case OP_POS:
        case OP_CAST:
            lower_un(j, op); pc++; break;
        case OP_INC: case OP_DEC:
            lower_incdec(j, op); pc++; break;
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_BW_AND: case OP_BW_OR: case OP_BW_XOR:
        case OP_DIV: case OP_MOD: case OP_SHL: case OP_SHR:
            lower_binop(j, op); pc++; break;
        case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE:
        case OP_LT: case OP_LE: case OP_CMP:
            lower_cmp(j, op); pc++; break;
        case OP_RETURN:
            lower_return(j, op);
            return trap_ret();
        case OP_INIT_FCALL: {
            /* start folded call; arg expressions run as normal ops until
             * SEND_vals then DO_FCALL (PLAN/ir-jit.md section 6) */
            if (j->p_callee) { j->fail = 1; return trap_fail(); }  /* nested */
            j->p_callee = (TFunc *)op->ic;
            j->p_argc = 0;
            pc++;
            break;
        }
        case OP_SEND_VAL: case OP_SEND_VAR: {
            if (!j->p_callee) { j->fail = 1; return trap_fail(); }
            if (j->p_argc >= 9) { j->fail = 1; return trap_fail(); }
            j->p_arg_slot[j->p_argc++] = op->a1;
            pc++;
            break;
        }
        case OP_DO_FCALL: {
            if (!j->p_callee) { j->fail = 1; return trap_fail(); }
            lower_call_finish(j, op);
            if (j->fail) return trap_fail();
            pc++;
            break;
        }
        case OP_JMP: {
            uint32_t T = op->ar;
            if (j->in_loop && T == j->loop_header)
                return trap_lb();   /* patched by loop-condition handler */
            return trap_end(ir_END(), T);
        }
        case OP_JMPZ: case OP_JMPNZ: {
            uint32_t T = op->ar;
            if (T <= pc) return trap_fail();

            /* loop condition: first JMPZ at loop top level */
            if (j->in_loop && j->in_loop_cond) {
                j->in_loop_cond = 0;
                uint32_t E = T;
                ir_ref ta, pa, ad;
                ad = cond_addr(j, op->t1, op->a1, &ta, &pa);
                ir_ref truthy = make_truthy(j, ta, pa, ad);
                /* JMPZ: truthy → body; JMPNZ at header: reverse (rare) */
                ir_ref ifx;
                if (op->op == OP_JMPZ) ifx = ir_IF(truthy);
                else ifx = ir_IF(ir_NOT_B(truthy));
                ir_IF_TRUE(ifx);
                Trap tb = emit_seq(j, pc + 1, stops, nstops);
                if (tb.kind == TR_FAIL) return tb;
                if (tb.kind == TR_LOOPBACK) {
                    ir_ref le = ir_LOOP_END();
                    ir_MERGE_SET_OP(j->loop_ref, 2, le);
                    j->in_loop = 0;
                } else {
                    return trap_fail();   /* body must back-edge or fail */
                }
                ir_IF_FALSE(ifx);
                /* exit path continues at E inside the false region */
                j->in_loop = 0;
                pc = E;
                break;
            }

            uint32_t join, else_start;
            if (!find_join(j->fn, pc, T, &join, &else_start))
                return trap_fail();

            ir_ref ta, pa, ad;
            ad = cond_addr(j, op->t1, op->a1, &ta, &pa);
            ir_ref truthy = make_truthy(j, ta, pa, ad);
            int jmp_on_true = (op->op == OP_JMPNZ);
            ir_ref ifx = ir_IF(jmp_on_true ? ir_NOT_B(truthy) : truthy);

            ir_IF_TRUE(ifx);
            Trap tA;
            if (jmp_on_true) {
                tA = trap_end(ir_END(), T);
            } else {
                uint32_t st[11]; int ns = nstops + 1;
                if (nstops > 10) return trap_fail();
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = T;
                tA = emit_seq(j, pc + 1, st, ns);
                if (tA.kind == TR_FAIL) return tA;
                if (tA.kind == TR_LOOPBACK) return trap_fail();
                if (tA.kind == TR_END && tA.target != join &&
                    !(else_start && tA.target == else_start))
                    return trap_fail();
            }

            ir_IF_FALSE(ifx);
            Trap tB;
            if (!jmp_on_true) {
                if (!else_start) {
                    tB = trap_end(ir_END(), T);
                } else {
                    uint32_t st[11]; int ns = nstops + 1;
                    if (nstops > 10) return trap_fail();
                    if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                    st[nstops] = join;
                    tB = emit_seq(j, else_start, st, ns);
                    if (tB.kind == TR_FAIL) return tB;
                    if (tB.kind == TR_LOOPBACK) return trap_fail();
                    if (tB.kind == TR_END && tB.target != join)
                        return trap_fail();
                }
            } else {
                uint32_t st[11]; int ns = nstops + 1;
                if (nstops > 10) return trap_fail();
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = T;
                tB = emit_seq(j, pc + 1, st, ns);
                if (tB.kind == TR_FAIL) return tB;
                if (tB.kind == TR_LOOPBACK) return trap_fail();
                if (tB.kind == TR_END && tB.target != join &&
                    !(else_start && tB.target == else_start))
                    return trap_fail();
            }

            int a_end = (tA.kind == TR_END && tA.target == join);
            int b_end = (tB.kind == TR_END && tB.target == join);
            if (tA.kind == TR_RET && tB.kind == TR_RET)
                return trap_ret();   /* both sides returned: dead join */
            if (tA.kind == TR_RET && b_end) {
                ir_BEGIN(tB.end);
                pc = join;
                break;
            }
            if (tB.kind == TR_RET && a_end) {
                ir_BEGIN(tA.end);
                pc = join;
                break;
            }
            if (a_end && b_end) {
                ir_MERGE_2(tA.end, tB.end);
                pc = join;
                break;
            }
            return trap_fail();
        }
        default:
            return trap_fail();
        }
    }
}

/* ================================================================
 * Compile entry
 * ================================================================ */
void tphp_jit_compile(TFunc *fn) {
    if (!g_jit_on || !fn) return;
    if (fn->jit_entry) return;
    if (!tphp_jit_eligible(fn)) {
        fn->hot = UINT16_MAX;
        return;
    }
    jit_lock();
    if (fn->jit_entry) { jit_unlock(); return; }

    JitC j;
    memset(&j, 0, sizeof j);
    j.fn = fn;
    ir_ctx *ctx = tm_malloc(sizeof(ir_ctx));
    memset(ctx, 0, sizeof *ctx);
    j.ctx = ctx;
    ir_init(ctx, IR_FUNCTION | IR_OPT_FOLDING,
            IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
    ctx->ret_type = IR_I32;

    /* IR requires START before PARAMs (see deps/ir/examples 0001/0008) */
    ir_START();
    j.r_vm = ir_PARAM(IR_ADDR, "vm", 1);
    j.r_slots = ir_PARAM(IR_ADDR, "slots", 2);
    j.r_args = ir_PARAM(IR_ADDR, "args", 3);
    j.r_ret = ir_PARAM(IR_ADDR, "ret", 4);

    /* prologue: guard every param is INT (read-only pass), then move.
     * Guard-fail is a sink (RETURN 0); the pass side stays live. */
    for (uint16_t i = 0; i < fn->n_params; i++) {
        ir_ref av = ir_ADD_A(j.r_args, ir_CONST_ADDR((uintptr_t)i * TVAL_SZ));
        ir_ref tg = ir_LOAD_I32(ir_ADD_A(av, ir_CONST_ADDR(TVAL_TAG)));
        ir_ref ifg = ir_IF(ir_NE(tg, ir_CONST_I32(JT_INT)));
        ir_IF_TRUE(ifg);
            ir_RETURN(ir_CONST_I32(0));
        ir_IF_FALSE(ifg);
    }
    for (uint16_t i = 0; i < fn->n_params; i++) {
        ir_ref av = ir_ADD_A(j.r_args, ir_CONST_ADDR((uintptr_t)i * TVAL_SZ));
        ir_ref sv = slot_addr(&j, i);
        store_pay(&j, sv, ir_LOAD_I64(av));
        store_tag(&j, sv, ir_LOAD_I32(ir_ADD_A(av, ir_CONST_ADDR(TVAL_TAG))));
        store_imm(&j, av, (int32_t)T_UNDEF, ir_CONST_I64(0));
    }

    uint32_t pc0 = 0;
    while (pc0 < fn->n_ops && fn->ops[pc0].op == OP_RECV) pc0++;

    Trap t = emit_seq(&j, pc0, NULL, 0);
    int ok = (!j.fail && t.kind == TR_RET);

    void *entry = NULL;
    if (ok) {
        size_t size = 0;
        entry = ir_jit_compile(ctx, 2, &size);
        if (jit_dbg())
            fprintf(stderr, "[jit] compile(%s) entry=%p size=%zu\n",
                    fn->name ? tstr_val(fn->name) : "?", entry, size);
    } else {
        if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0])
            fprintf(stderr, "[jit] compile(%s) FAILED emit (fail=%d trap=%d)\n",
                    fn->name ? tstr_val(fn->name) : "?", j.fail, (int)t.kind);
    }
    ir_free(ctx);
    tm_free(ctx);

    if (entry)
        fn->jit_entry = entry;
    else
        fn->hot = UINT16_MAX;
    jit_unlock();
}

#else /* !TPHP_JIT */

bool tphp_jit_available(void) { return false; }
void tphp_jit_init(void) {}
void tphp_jit_compile(TFunc *fn) { fn->hot = UINT16_MAX; }

#endif
