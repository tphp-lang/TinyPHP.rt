/*
 * TinyPHP.rt - a small PHP runtime
 * jit.c - IR baseline JIT (PLAN/ir-jit.md v1)
 *
 * Closed scalar world: eligible functions only touch int/float/bool/null.
 * Frame slots stay the single source of truth; IR loads/stores them and
 * lets IR's optimizer clean within-block redundancy. Slow paths call the
 * tval_* family (one semantic source).
 *
 * JitEntry ABI (PLAN/ir-jit.md §3, v1.2 direct-slot):
 *   int fn(VM *vm, TVal *slots, TVal *ret);
 *   slots[0..n_params-1] = params (INT guaranteed for JIT→JIT);
 *   1 = *ret owned by caller; 0 = deopt (interpreter re-runs).
 */
#include "value.h"
#include "opcode.h"
#include "tphp.h"
#include "class.h"
#include "jit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <uv.h>            /* uv_hrtime → compile 成本诊断（TPHP_JIT_DEBUG） */

void tphp_vm_frame_destroy(VMFrame *f);   /* vm.c */
TVal tphp_vm_call_callable(VM *vm, TVal callable, TVal *args, int n); /* vm.c */
int tphp_vm_call_guarded(VM *vm, TVal callable, TVal *args, int n, TVal *ret); /* vm.c 异常分发守卫版 */
TClosure *tphp_tclo_alloc0(TFunc *fn);                               /* vm.c */
int tphp_jit_call_userfn(VM *vm, TFunc *fn, TVal *args, int argc, TVal *ret); /* vm.c 刀19 慢路 */
TClosure *tphp_tclo_alloc(TFunc *fn, TVal *caps, uint16_t n_cap);    /* vm.c P0-18① */
int tphp_jit_opseq(VM *vm, TFunc *fn, TVal *slots, uint32_t pc, uint32_t pc_end, TVal *recv_sc); /* vm.c */
int tphp_jit_fetch_prop(VM *vm, TFunc *fn, TOp *op, uint32_t pc, TVal *slots); /* vm.c 刀5：假帧+真 handler */
int tphp_jit_spec_guard(TVal *base); /* vm.c 类型特化批1：preheader ARR+packed+全INT 一体查 */
int tphp_jit_spec_guard_ht(TVal *base); /* vm.c 类型特化批4a：ARR+hash+全活跃桶值 INT 一体查 */

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

/* IR backend is available on x86_64 and aarch64 (both vendored in deps/ir;
 * emit headers pre-generated: ir_emit_x86.h / ir_emit_aarch64.h). */
#define JIT_PLAT_OK 1

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

/* prop v1.5 批1：布局锁（PLAN/prop-ic.md §2.4）——TProp/TObj/TClass 布局
 * 变动即编译期炸，防 offsetof 手抄漂移。 */
_Static_assert(sizeof(TProp) == 80, "TProp size");
_Static_assert(offsetof(TObj, cls) == 24, "TObj.cls");
_Static_assert(offsetof(TObj, props) == 32, "TObj.props");
_Static_assert(offsetof(TClass, n_props) == 80, "TClass.n_props");
_Static_assert(offsetof(TClass, props) == 72, "TClass.props");
_Static_assert(offsetof(TProp, name) == 0, "TProp.name");
_Static_assert(offsetof(TProp, visibility) == 24, "TProp.visibility");
_Static_assert(offsetof(TProp, has_hooks) == 72, "TProp.has_hooks");
_Static_assert(offsetof(TProp, set_visibility) == 25, "TProp.set_visibility");
_Static_assert(offsetof(TProp, type) == 8, "TProp.type");
_Static_assert(offsetof(TProp, is_readonly) == 27, "TProp.is_readonly");
_Static_assert(offsetof(TClass, flags) == 28, "TClass.flags");
_Static_assert(offsetof(GCHead, kind) == 5, "GCHead.kind");

#define JT_NULL  ((int32_t)T_NULL)
#define JT_FALSE ((int32_t)T_FALSE)
#define JT_TRUE  ((int32_t)T_TRUE)
#define JT_INT   ((int32_t)T_INT)
#define JT_FLOAT ((int32_t)T_FLOAT)

/* ---- slow helpers ----
 * P0-12 残留跨 IR raise 修复：raise 源自 tval_* 内部（vm_value_raise →
 * longjmp），无法后置到冷分支——与 bcall 同构：入口事前 setjmp 登记，
 * raise 落本地 mine → restore → 返回 0 让 IR deopt（不跨 IR 帧）。
 * 成本仅在慢路径（类型分发本身 ≥20ns），both-int 快路径不触。
 * 1 = 正常，0 = raise landed（exception 已设，IR RETURN 0 传播）。 */
static void binop_slow_body(VM *vm, int32_t opi, TVal *dst,
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

static void binop_slow_body(VM *vm, int32_t opi, TVal *dst,
                         int32_t ta, int64_t pa, int32_t tb, int64_t pb);
static void cmp_slow_body(VM *vm, int32_t opi, TVal *dst,
                          int32_t ta, int64_t pa, int32_t tb, int64_t pb);

int tphp_jit_binop_slow(VM *vm, int32_t opi, TVal *dst,
                        int32_t ta, int64_t pa, int32_t tb, int64_t pb) {
    jmp_buf outer, mine;
    memcpy(&outer, &vm->x.unwind, sizeof(jmp_buf));
    if (TPHP_SETJMP(mine) == 0) {
        memcpy(&vm->x.unwind, &mine, sizeof(jmp_buf));
        binop_slow_body(vm, opi, dst, ta, pa, tb, pb);
        memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
        return 1;
    }
    memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
    return 0;
}

int tphp_jit_cmp_slow(VM *vm, int32_t opi, TVal *dst,
                       int32_t ta, int64_t pa, int32_t tb, int64_t pb) {
    jmp_buf outer, mine;
    memcpy(&outer, &vm->x.unwind, sizeof(jmp_buf));
    if (TPHP_SETJMP(mine) == 0) {
        memcpy(&vm->x.unwind, &mine, sizeof(jmp_buf));
        cmp_slow_body(vm, opi, dst, ta, pa, tb, pb);
        memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
        return 1;
    }
    memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
    return 0;
}

static void cmp_slow_body(VM *vm, int32_t opi, TVal *dst,
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

/* CAST 慢路（刀2）：快叶收窄后其余形态落此——ct==1 的 FLOAT（tval_to_int
 * 对 NAN/超范围 raise，快叶 FP2I64 裸转不查=错值）与 STR/ARR/OBJ 等、
 * ct==3 的 STR/ARR/OBJ 等。顺序严格 compute-then-release：dst 可与源
 * 同槽（$s=(int)$s），先转换后释放 → 别名下无 UAF；raise 落本地 mine →
 * restore → 返 0 让 IR deopt（binop_slow 同款，绝不跨 IR 帧 longjmp）。
 * UNDEF 源经 tval_to_* 默认支 raise TypeError——与 binop_slow 读原始
 * UNDEF 的既存发散同族（解释器 rdv 抛 UndefinedError），记观察不扩刀。 */
static void cast_slow_body(VM *vm, int32_t ct, TVal *dst,
                           int32_t ta, int64_t pa) {
    (void)vm;
    TVal a = { .u.i = pa, .t = (TType)ta };
    TVal r;
    switch (ct) {
    case 1:  r = TV_INT(tval_to_int(a)); break;
    case 3:  r = TV_FLOAT(tval_to_float(a)); break;
    default: r = tval_to_bool(a) ? TV_TRUE : TV_FALSE; break;
    }
    tval_release(dst);
    *dst = r;
}

int tphp_jit_cast_slow(VM *vm, int32_t ct, TVal *dst,
                       int32_t ta, int64_t pa) {
    jmp_buf outer, mine;
    memcpy(&outer, &vm->x.unwind, sizeof(jmp_buf));
    if (TPHP_SETJMP(mine) == 0) {
        memcpy(&vm->x.unwind, &mine, sizeof(jmp_buf));
        cast_slow_body(vm, ct, dst, ta, pa);
        memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
        return 1;
    }
    memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
    return 0;
}

/* Lookup jit_entry at runtime and call with pre-built slots (v1.2).
 * Needed because recursive/self calls compile before jit_entry is set. */
int tphp_jit_call_slots(VM *vm, TFunc *fn, TVal *slots, TVal *ret) {
    JitEntry e = (JitEntry)fn->jit_entry;
    if (!e) return 0;
    /* 批5: 参数 INT 契约（v1.2 "caller guarantees INT"）在非 INT 实参的
     * 静态调用上从未被踩过（logstat 批3 进 JIT 后首次）——非 INT → 返 0
     * 走 caller 既有 deopt 链（IF(ok==0) RETURN 0 → 解释器接管） */
    for (uint16_t i = 0; i < fn->n_params; i++)
        if (slots[i].t != T_INT) return 0;
    return e(vm, slots, ret);
}

/* JIT->JIT call trampoline (section 8): TLS slot stack - closed-world
 * callees never raise and never touch vm->x.frame. Recursion safe via
 * bump pointer (fib depth ~28 x ~10 slots fits easily).
 * v1.2: params written directly to slots[0..n]; no args_in. */
int tphp_jit_call(VM *vm, TFunc *fn, TVal *args_in, TVal *ret) {
    JitEntry e = (JitEntry)fn->jit_entry;
    if (!e) return 0;
    uint32_t nparams = fn->n_params;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    enum { JSIZE = 2048 };
    static _Thread_local TVal g_jstack[JSIZE];
    static _Thread_local uint32_t g_jsp;
    if (g_jsp + nslots > JSIZE) return 0;
    TVal *slots = &g_jstack[g_jsp];
    g_jsp += nslots;
    /* write params directly; init locals/tmps to UNDEF */
    for (uint32_t i = 0; i < nparams; i++)
        slots[i] = args_in[i];
    for (uint32_t i = nparams; i < nslots; i++)
        slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    int ok = e(vm, slots, ret);
    g_jsp -= nslots;
    return ok;
}

/* ---- v1.1 helpers：镜像解释器语义（可 raise；raise 由调用点的
 * vm->x.unwind 接住——解释器钩子/FFI bounded pad 均在 C 栈上方） ---- */

void tphp_jit_array_init(VM *vm, TVal *dst) {
    (void)vm;
    tval_release(dst);
    *dst = TV_ARR(tarr_new());
}

/* 冷路径 raise 守卫（P0-10）：setjmp 接住 tphp_vm_raise 的 longjmp 并恢复
 * 调用方的 unwind 目标——raise 永不跨 IR 帧展开（IR 机器码无 UNWIND_INFO，
 * RtlUnwind 跨它即 0xC0000005/0xC00000FF，2026-10-28 cdb/gdb 实测）。
 * 仅供错误冷路径调用（热路径成功段禁止 setjmp：实测 array-push +24ms）。 */
static void tphp_raise_guarded(VM *vm, const char *cls, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void tphp_raise_guarded(VM *vm, const char *cls, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    jmp_buf outer, mine;
    memcpy(&outer, &vm->x.unwind, sizeof(jmp_buf));
    if (TPHP_SETJMP(mine) == 0) {
        memcpy(&vm->x.unwind, &mine, sizeof(jmp_buf));
        tphp_vm_raise(vm, cls, "%s", buf);   /* noreturn：longjmp 落回 mine */
    }
    memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
}

/* CONCAT（P0-14 审计最大户）：镜像 op_concat（.= 原地 append 快路由 emit
 * 传 inplace，编译期判同槽）。快慢路分离（P0-10 教训）：
 * - 快路 = 两端 ∈ {STR,INT,FLOAT,BOOL,NULL}（tval_to_str 字面量集无
 *   raise 面）——零 setjmp，覆盖 `$out .=`/`"v".$i` 等 concat 主流；
 * - 慢路（OBJ/ARR/RES/UNDEF 混入，tostr hook 可 raise）→ 事前 setjmp
 *   wrapper（binop_slow 同款，每次拼接都付但仅冷形态）。
 * 1 = 正常，0 = raise landed（IR deopt → 调用侧 RETURN 0 → P0-12 链）。 */
int tphp_jit_concat(VM *vm, TVal *dst, TVal *a, TVal *b, int inplace) {
    TVal av = *a, bv = *b;
    bool fast = (av.t == T_STR || av.t == T_INT || av.t == T_FLOAT ||
                 av.t == T_TRUE || av.t == T_FALSE || av.t == T_NULL) &&
                (bv.t == T_STR || bv.t == T_INT || bv.t == T_FLOAT ||
                 bv.t == T_TRUE || bv.t == T_FALSE || bv.t == T_NULL);
    if (fast) {
        if (inplace && av.t == T_STR && bv.t == T_STR &&
            tstr_append_inplace((TStr *)av.u.p, ((TStr *)bv.u.p)->c,
                                ((TStr *)bv.u.p)->len))
            return 1;   /* 同槽原地成功：dst 即 a 槽，值已更新（op_concat 同） */
        TVal r = tval_concat(av, bv);
        tval_release(dst);
        *dst = r;
        return 1;
    }
    {
        jmp_buf outer, mine;
        memcpy(&outer, &vm->x.unwind, sizeof(jmp_buf));
        if (TPHP_SETJMP(mine) == 0) {
            memcpy(&vm->x.unwind, &mine, sizeof(jmp_buf));
            TVal r = tval_concat(av, bv);
            memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
            tval_release(dst);
            *dst = r;
            return 1;
        }
        memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
        return 0;   /* raise：dst 不动，IR deopt */
    }
}

/* ARRAY_APPEND（array literal 元素，P0-1）：镜像 do_dim_write 的
 * append/keyed 两形态——dim_cow + addref val + tarr_set(内部 addref key)。
 * key==NULL 为 append 形态（t2==UNUSED）。base 恒为紧邻 ARRAY_INIT 的数组，
 * 但键表达式可为运行期数组（`[$k => 1]`）→ norm_key 可 raise。
 * 快慢路分离（P0-10 性能教训）：成功路径零 setjmp（热循环不付
 * setjmp+2×jmp_buf memcpy ≈20ns/call，array-push 实测 +24ms）；
 * 仅错误冷路径进守卫段。1 = 正常，0 = raise landed（IR deopt）。 */
int tphp_jit_array_append(VM *vm, TVal *base, TVal *key, TVal *val) {
    TVal k;
    TVal *kp = NULL;
    /* 检查顺序镜像解释器 do_dim_write：base 类型优先于 key norm */
    if (base->t == T_UNDEF)
        *base = TV_ARR(tarr_new());          /* UNDEF 非 rc，免释放 */
    if (base->t != T_ARR) {
        tphp_raise_guarded(vm, "TypeError",
                           "Cannot use a scalar value as an array");
        return 0;
    }
    if (key) {
        k = *key;
        if (!tval_norm_key(&k)) {
            tphp_raise_guarded(vm, "TypeError", "Illegal offset type");
            return 0;
        }
        kp = &k;
    }
    {
        TArr *a = tarr_cow((TArr *)base->u.p);
        if (a != (TArr *)base->u.p) {
            tval_release(base);
            *base = TV_ARR(a);
        }
        tval_addref(*val);
        if (kp) tarr_set(a, *kp, *val);
        else    tarr_append(a, *val);
    }
    return 1;
}

/* 带键/无键 ASSIGN_DIM（P0-10，$a[$k] = $v / $a[] = $v）：镜像 do_dim_write。
 * base UNDEF → auto-viv；T_STR 报 string-offset（M1 文案）；其余非数组报
 * TypeError；key==NULL 为 append 形态。检查顺序与解释器一致：base 类型
 * 优先于 key norm。FFI CData 分支不可达（CData 仅 FFI ops 产生，不在白名单）。
 * 快慢路分离：成功路径零 setjmp（热循环 1e6 次 append 实测 setjmp 版
 * +24ms），错误冷路径进 raise_guarded——longjmp 不跨 IR 帧。
 * 1 = 正常，0 = raise landed（IR deopt 传播）。 */
int tphp_jit_assign_dim(VM *vm, TVal *base, TVal *key, TVal *val) {
    TVal k;
    TVal *kp = NULL;
    if (base->t == T_UNDEF)
        *base = TV_ARR(tarr_new());            /* auto-viv（UNDEF 非 rc，免释放） */
    if (base->t == T_STR) {
        tphp_raise_guarded(vm, "Error",
                           "Cannot use a string offset as an array in M1");
        return 0;
    }
    if (base->t != T_ARR) {
        tphp_raise_guarded(vm, "TypeError",
                           "Cannot use a scalar value as an array");
        return 0;
    }
    if (key) {
        k = *key;
        if (!tval_norm_key(&k)) {
            tphp_raise_guarded(vm, "TypeError", "Illegal offset type");
            return 0;
        }
        kp = &k;
    }
    {
        TArr *a = tarr_cow((TArr *)base->u.p);
        if (a != (TArr *)base->u.p) {
            tval_release(base);
            *base = TV_ARR(a);
        }
        tval_addref(*val);
        if (kp) tarr_set(a, *kp, *val);
        else    tarr_append(a, *val);          /* append 形态（key==NULL） */
    }
    return 1;
}

/* OP_CONST 字符串字面量（P0-1）：镜像 op_const——addref（lit_str 恒
 * intern → no-op）+ 释放旧 dst（wtake 语义）+ 落值。标量走 store_imm 快路。 */
void tphp_jit_lit_set(TVal *dst, TVal *lit) {
    TVal v = *lit;
    tval_addref(v);
    tval_release(dst);
    *dst = v;
}

/* OP_COPY/OP_ASSIGN（P0-1）：镜像 op_copy——addref src + release 旧 dst。
 * 标量两操作皆 no-op，仅付一次 C 调用；引用计数值（数组/串）否则会
 * 双重释放（tmp-rc 探针 0xC0000409 实测）。 */
void tphp_jit_tv_copy(TVal *dst, TVal *src) {
    TVal v = *src;
    tval_addref(v);
    tval_release(dst);
    *dst = v;
}

/* FETCH_DIM_R：镜像 op_fetch_dim_r（P0-12：raise 全走 raise_guarded 冷
 * 守卫——成功路径零 setjmp，热路径（array-read 1e6 fetch）无成本）。
 * 补 string offset 读分支（原版直接 raise 既跨 IR 又错语义：解释器
 * $s[0] 返回字符，原版报 TypeError）——镜像解释器 string 分支。
 * FFI CData base 仍落 "value" TypeError（FFI ops 不进白名单，边界记录）。
 * 1 = 正常，0 = raise landed（IR deopt）。 */
int tphp_jit_fetch_dim_r(VM *vm, TVal *dst, TVal *base, TVal *key_in, DimIC *ic) {
    TVal base_v = *base;
    TVal key = *key_in;
    if (base_v.t == T_STR) {
        /* 镜像解释器 string offset 分支（vm.c op_fetch_dim_r） */
        TStr *s = (TStr *)base_v.u.p;
        if (!tval_norm_key(&key) || key.t != T_INT) {
            tphp_raise_guarded(vm, "TypeError",
                               "Cannot access offset of type %s on string",
                               key.t == T_INT ? "int" : key.t == T_STR ? "string" : "unsupported");
            return 0;
        }
        int64_t i = key.u.i;
        if (i < 0) i += s->len;
        if (i < 0 || i >= (int64_t)s->len) {
            tphp_raise_guarded(vm, "UndefinedError",
                               "Uninitialized string offset %lld",
                               (long long)key.u.i);
            return 0;
        }
        tval_release(dst);
        *dst = TV_STR(tstr_new(s->c + i, 1));   /* tstr_new rc=1 直属 dst */
        return 1;
    }
    if (base_v.t != T_ARR) {
        tphp_raise_guarded(vm, "TypeError",
                           "Cannot access offset on %s",
                           base_v.t == T_NULL ? "null" : base_v.t == T_INT ? "int" :
                           base_v.t == T_FLOAT ? "float" :
                           (base_v.t == T_TRUE || base_v.t == T_FALSE) ? "bool" : "value");
        return 0;
    }
    {
        TArr *arr = (TArr *)base_v.u.p;
        if (!tval_norm_key(&key)) {
            tphp_raise_guarded(vm, "TypeError", "Illegal offset type");
            return 0;
        }
        TVal *v = tarr_get_ic(arr, key, ic);   /* P0-13: per-site IC（emit 侧懒分配） */
        if (!v) {
            if (key.t == T_INT)
                tphp_raise_guarded(vm, "UndefinedError", "Undefined array key %lld",
                                   (long long)key.u.i);
            else
                tphp_raise_guarded(vm, "UndefinedError", "Undefined array key \"%s\"",
                                   tstr_val((TStr *)key.u.p));
            return 0;
        }
        tval_addref(*v);
        tval_release(dst);
        *dst = *v;
    }
    return 1;
}

/* CLOSURE_INIT：镜像 op_closure_init（P0-18①：n_cap>0 弹 caps 栈，配对由
 * 编译器保证；underflow 与 fn 越界同属内部不变量 raise，记录在案边界） */
void tphp_jit_closure_init(VM *vm, TVal *dst, TFunc *self, uint32_t fn_idx,
                           uint16_t n_cap) {
    Program *prog = self->prog ? self->prog : vm->prog;
    if (!prog || fn_idx >= (uint32_t)prog->n_funcs)
        tphp_vm_raise(vm, "Error", "Internal: closure function missing");
    TFunc *fn = prog->funcs[fn_idx];
    TClosure *c;
    if (n_cap) {
        if (vm->x.n_caps < n_cap)
            tphp_vm_raise(vm, "Error", "Internal: capture stack underflow");
        c = tphp_tclo_alloc(fn, &vm->x.caps[vm->x.n_caps - n_cap], n_cap);
        for (uint16_t i = 0; i < n_cap; i++)
            tval_release(&vm->x.caps[vm->x.n_caps - n_cap + i]);
        vm->x.n_caps -= n_cap;
    } else {
        c = tphp_tclo_alloc0(fn);
    }
    tval_release(dst);
    *dst = TV_CLO(c);
}

/* SEND_CAP（P0-18①）：镜像 op_send_cap——raw 读源（捕获未定义变量→null），
 * addref 后压 vm->x.caps 栈。唯一 raise 面 = 上限 256（Too many closure
 * captures，真实可触发的 PHP 语义）→ setjmp 守卫（P0-12 模式，longjmp
 * 永不跨 IR 帧），检查在 addref 之前——超限零副作用，IR deopt 后
 * op_do_fcall 层见 exception 走异常传播不重跑（无重复 push 风险）。
 * 1 = 正常，0 = raise landed（emit 侧 IF + RETURN 0，concat 同款）。 */
int tphp_jit_send_cap(VM *vm, TVal *src) {
    if (vm->x.n_caps >= 256) {
        tphp_raise_guarded(vm, "Error", "Too many closure captures");
        return 0;
    }
    TVal v = *src;
    if (v.t == T_UNDEF) v = TV_NULL;
    tval_addref(v);
    vm->x.caps[vm->x.n_caps++] = v;
    return 1;
}

/* INIT_DYN..DO_FCALL 动态调用（$f(...)）：JIT'd 无捕获闭包走 trampoline，
 * 其余回落 tphp_vm_call_guarded（语义统一入口 + 异常分发守卫，vm.c）。
 * 返回 0 = callee deopt（exception 已设）→ IR 立即 RETURN 0 沿链传播；
 * 不可落回解释器重跑（双重副作用 + exception 悬挂）。
 * tphp_jit_call 返 0 但无 exception（!entry / g_jsp 溢出，未执行）→
 * 仍走解释器 fallback。 */
int tphp_jit_call_dyn(VM *vm, TVal *clo_slot, TVal *args_in, int n, TVal *ret) {
    TVal clo = *clo_slot;
    if (clo.t == T_CLOSURE) {
        TClosure *c = (TClosure *)clo.u.p;
        /* P0-15 C：合并 call_dyn→tphp_jit_call 两层 C 帧；entry 查表提前；
         * 补 n==nparams 防越界（原版直接拷 nparams 个，n<nparams 读越界）。
         * P0-18①：n_cap 放开——帧布局 [captures 0..n_cap) + [CV n_cap..)，
         * 捕获 addref 拷入（帧槽所有权，op_do_fcall 直入同构：callee wt
         * 覆盖时引用随写随变，返回后统一 release 当前值，无双释放）；
         * args 仍借用（真引用在调用方 scratch）。 */
        if (c->fn && !c->bfn) {
            JitEntry e = (JitEntry)c->fn->jit_entry;
            if (e) {
                uint32_t n_cap = c->n_cap;
                uint32_t nparams = c->fn->n_params;
                uint32_t nslots = (uint32_t)c->fn->n_locals + c->fn->n_tmps;
                enum { JSIZE = 2048 };
                static _Thread_local TVal g_jstack[JSIZE];
                static _Thread_local uint32_t g_jsp;
                if (nparams == (uint32_t)n && g_jsp + nslots <= JSIZE) {
                    TVal *slots = &g_jstack[g_jsp];
                    g_jsp += nslots;
                    for (uint32_t i = 0; i < n_cap; i++) {
                        tval_addref(c->captures[i]);
                        slots[i] = c->captures[i];
                    }
                    for (uint32_t i = 0; i < nparams; i++)
                        slots[n_cap + i] = args_in[i];
                    for (uint32_t i = n_cap + nparams; i < nslots; i++)
                        slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
                    int ok = e(vm, slots, ret);
                    g_jsp -= nslots;
                    /* 捕获引用归还（含异常/未执行路径——addref 发生在 e()
                     * 之前，无论执行与否 slots[0..n_cap) 当前值都须释放，
                     * 否则 g_jstack 回退即泄漏） */
                    for (uint32_t i = 0; i < n_cap; i++)
                        tval_release(&slots[i]);
                    if (ok) return 1;
                    if (vm->x.exception)
                        return 0;
                }
            }
        }
    }
    /* 解释器 callee → guarded 调用（vm.c:5637 注详）：本地 pad +
     * vm_unwind_core 以 floor 分发——callee 侧 handler resume 跑完返 1，
     * 逃逸返 0（IR RETURN 上传，零跨帧展开）。两面封死：bug1（裸调的
     * longjmp 穿无 UNWIND_INFO 的 IR 帧 → 0xC00000FF）+ bug2（缺分发
     * 把 callee-try 异常外传 → caller JIT 已退出后 RECV 重跑 arity 误报）。 */
    if (tphp_vm_call_guarded(vm, clo, args_in, n, ret))
        return 1;
    return 0;
}

/* INIT_FCALL..DO_FCALL builtin 调用（P0-1：闭包尾部校验 echo→print 等
 * 形态进 JIT）：argv 按值借用（槽位持有真引用，scratch 无需释放），
 * 返回值 +1 转移给 dst——与解释器 DO_FCALL builtin 分支同语义。
 * builtin raise 由本地 setjmp 接住（vm->x.exception 保留），返回 0 让
 * IR 端走 deopt（return 0 退出 JIT）——**longjmp 绝不跨 IR 机器码帧**：
 * ucrt 的 longjmp 走 RtlUnwind SEH 展开，IR 帧无 UNWIND_INFO，跨它
 * 即 0xC00000FF 栈溢出（2026-10-28 cdb/gdb 实测定位）。返回 1 = 正常。 */
int tphp_jit_bcall(VM *vm, void *bfn, int argc, TVal *argv, TVal *ret) {
    TVal (*bf)(VM *, TVal *, int) = (TVal (*)(VM *, TVal *, int))bfn;
    jmp_buf outer, mine;
    memcpy(&outer, &vm->x.unwind, sizeof(jmp_buf));
    if (TPHP_SETJMP(mine) == 0) {
        memcpy(&vm->x.unwind, &mine, sizeof(jmp_buf));
        *ret = bf(vm, argv, argc);
        memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
        return 1;
    }
    /* raise landed here: keep vm->x.exception, restore the interpreter's
     * target, report failure so IR deopts out (exit via normal return) */
    memcpy(&vm->x.unwind, &outer, sizeof(jmp_buf));
    return 0;
}
/* ================================================================
 * Eligibility (PLAN/ir-jit.md §5)
 * ================================================================ */
static bool jit_eligible_inner(TFunc *fn, int depth);

static bool lit_scalar(TVal v) {
    return v.t == T_INT || v.t == T_FLOAT || v.t == T_TRUE ||
           v.t == T_FALSE || v.t == T_NULL;
}

/* CFG 守卫（v1.1）：曾用于拒绝嵌套循环/循环内 if-else（IR codegen 崩溃）。
 * 2026-10-27 根因已修：deps/ir IR_BITSET_ONE 在 Windows LLP64 下 1UL<<32
 * 回绕置位 bit0，污染 block scheduler bitset（3470e7e）。守卫现只保留
 * 深度护栏（>16 层回跳头即拒，nh>=16 处）——嵌套循环与循环内 if-else
 * 不再拒绝（曾漏改末行 nh<=1，array-read/kv-read 双层循环因此从不进
 * JIT，2026-10-28 修复）。 */
static int find_join(TFunc *fn, uint32_t p, uint32_t T,
                     uint32_t *join_out, uint32_t *else_out);
static bool jit_cfg_ok(TFunc *fn) {
    uint32_t hdr[16]; int nh = 0;
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if (op->op != OP_JMP) continue;
        uint32_t H = op->ar;
        if (H > i) continue;            /* 只看回跳 */
        int seen = 0;
        for (int k = 0; k < nh; k++) if (hdr[k] == H) seen = 1;
        if (!seen) {
            if (nh >= 16) return false;
            hdr[nh++] = H;
        }
    }
    return true;                        /* 深度护栏已在 nh>=16 生效 */
}

/* 批3-3：INIT_SMCALL 编译期解析——op_init_smcall 运行期语义中**静态可判
 * 子集**的镜像（kind 0 self::=caller->decl_cls / 1 parent::=decl->parent /
 * 3 字面量 Cls::m；static::(LSB)/动态名/enum builtin/native/__callStatic/
 * __construct/实例转发 全拒 = 回退解释器走完整语义）。可见性用与运行期
 * 同一 tphp_vis_ok，from=caller->decl_cls——运行期 scope（called_cls 链）
 * 恒为 decl 或其子类，vis_ok 链式规则下编译期判定**恒不松于**运行期，
 * 拒掉即回解释器，无放行偏差。
 * 成功 = callee TFunc*，同时缓存进 op->ic（op_init_smcall 不读 ic，与批2
 * INIT_DYN 同款零读者直用；对齐指针原子写，并发同值写无撕裂面）。
 * 只查不 autoload（tphp_class_lookup）：类未加载 → 拒，解释器路径处理。 */
static TFunc *jit_smcallee(TFunc *caller, TOp *op, const char **rej) {
    if (op->t1 != OPD_UNUSED) { *rej = "SMCALL dyn name"; return NULL; }
    int kind = (int)op->a1;
    if (kind == 2) { *rej = "SMCALL static:: LSB"; return NULL; }
    TClass *cls;
    TStr *mn;
    if (kind == 0) {
        cls = caller->decl_cls;
        if (!cls) { *rej = "self:: outside class"; return NULL; }
        mn = (TStr *)caller->lits[op->ar].u.p;
    } else if (kind == 1) {
        cls = caller->decl_cls;
        if (!cls || !cls->parent) { *rej = "parent:: outside child"; return NULL; }
        cls = cls->parent;
        mn = (TStr *)caller->lits[op->ar].u.p;
    } else {
        const char *s = tstr_val((TStr *)caller->lits[op->ar].u.p);
        const char *sep = NULL;
        for (const char *p = s; (p = strstr(p, "::")) != NULL; p += 2) sep = p;
        if (!sep) { *rej = "SMCALL malformed key"; return NULL; }
        char clc[128];
        size_t cl = (size_t)(sep - s);
        if (cl >= sizeof clc) cl = sizeof clc - 1;
        for (size_t i = 0; i < cl; i++)
            clc[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
        clc[cl] = 0;
        cls = tphp_class_lookup(clc);
        if (!cls) { *rej = "SMCALL class not loaded"; return NULL; }
        mn = tstr_intern(sep + 2, strlen(sep + 2));
    }
    tphp_class_ensure_flat(cls);
    TMethod *m = tphp_class_find_method(cls, tstr_val(mn));
    if (!m || !m->fn || !m->is_static || m->is_abstract)
        { *rej = "SMCALL not user-static method"; return NULL; }
    if (!tphp_vis_ok(m->visibility, m->decl ? m->decl : cls,
                     caller->decl_cls))
        { *rej = "SMCALL visibility"; return NULL; }
    return m->fn;
}

static bool jit_eligible_inner(TFunc *fn, int depth) {
    if (depth > 16) return false;
    if (!fn || fn->is_main ||
        fn->is_variadic || fn->is_generator || fn->n_ex > 0 ||
        fn->n_params > 8)
        return false;   /* is_closure 允许；n_cap>0 放开（P0-18①）：捕获槽
                         * [0..n_cap) 由帧建立机制预填（vm.c frame push
                         * rec->clo 分支），闭包体内读捕获=普通槽读（emit 层
                         * 零改动）；prologue pc0 跳 RECV 不受影响（RECV 目标
                         * 槽=编译期 n_cap+idx）；定义点 SEND_CAP/CLOSURE_INIT
                         * 同批放行。is_method 批2-2 解禁：$this@slot0/参数
                         * 1.. 布局由 op_do_fcall 直入 poff 承接（vm.c）——
                         * 方法进 JIT 唯一入口就是该直入（INIT_FCALL 不解析
                         * 方法，JIT→JIT 到不了方法体），旧禁令=该布局债的
                         * 另一半 */
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
    const char *rej = NULL;          /* P0-14 审计：拒因（TPHP_JIT_DEBUG） */
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        switch (op->op) {
        case OP_RECV:
            if (i != recv_seen) { rej = "RECV not leading"; goto out; }
            recv_seen++;
            if (recv_seen > fn->n_params) { rej = "extra RECV"; goto out; }
            break;
        case OP_CONST: {
            TVal v = fn->lits[op->a1];
            /* T_STR 放行（P0-1 收尾）：array-read/kv-read 的 echo/键串,
             * lit_str 恒 intern → addref/release 皆 no-op，按值存槽安全 */
            if (!lit_scalar(v) && v.t != T_STR) { rej = "CONST non-scalar lit"; goto out; }
            break;
        }
        case OP_COPY: case OP_ASSIGN: case OP_INC: case OP_DEC:
        case OP_NEG: case OP_POS: case OP_BOOL_NOT: case OP_BOOL_VAL:
        case OP_ARRAY_INIT:
            break;
        case OP_ASSIGN_DIM:
            /* append 形态（$a[] = $v）+ 带键形态（P0-10，$a[$k] = $v）：
             * base 必为槽；key/val 可为槽或标量/串字面量（emit 物化） */
            if (op->t1 == OPD_CONST || op->t1 == OPD_UNUSED) { rej = "ASSIGN_DIM base/形态"; goto out; }
            if (op->t2 != OPD_UNUSED && op->t2 == OPD_CONST &&
                !lit_scalar(fn->lits[op->a2]) && fn->lits[op->a2].t != T_STR)
                { rej = "ASSIGN_DIM key lit"; goto out; }
            if (op->tr == OPD_CONST &&
                !lit_scalar(fn->lits[op->ar]) && fn->lits[op->ar].t != T_STR)
                { rej = "ASSIGN_DIM val lit"; goto out; }
            break;
        case OP_FETCH_DIM_R:
            if (op->t2 == OPD_UNUSED) { rej = "FETCH_DIM_R no key"; goto out; }
            break;
        case OP_SEND_CAP:
            /* P0-18①：源恒为变量 Loc（compile.c use 序列：CV 直取或
             * FETCH_GLOBAL→TMP），raw 读语义（UNDEF→null）在 helper 内 */
            if (op->t1 != OPD_CV && op->t1 != OPD_TMP)
                { rej = "SEND_CAP src not slot"; goto out; }
            break;
        case OP_CLOSURE_INIT:
            /* captured（a2>0）放开（P0-18①）：SEND_CAP/CLOSURE_INIT 配对
             * 由编译器保证（同一 uses 序列），helper 弹 caps 栈镜像
             * op_closure_init */
            break;
        case OP_INIT_DYN:
            break;
        case OP_CAST: {
            if (op->t2 != OPD_CONST) { rej = "CAST non-ct"; goto out; }
            int64_t ct = fn->lits[op->a2].u.i;
            if (ct != 1 && ct != 2 && ct != 3) { rej = "CAST type(int/bool/float only)"; goto out; }
            break;
        }
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_BW_AND: case OP_BW_OR: case OP_BW_XOR:
        case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE:
        case OP_LT: case OP_LE: case OP_CMP:
            break;
        case OP_DIV: case OP_MOD:
            /* 编译器把字面量先物化进槽（trace：CONST; MOD 槽对槽）——t2 恒
             * 槽，旧 t2==CONST 检查形同虚设地过严（P0-14 审计）。零除由
             * slow helper raise + P0-12 deopt 兜底，槽形态全放行；仅保留
             * 字面量除数编译期可判的零（防御万一某路径产 const 形态） */
            if (op->t2 == OPD_CONST) {
                TVal d = fn->lits[op->a2];
                if (d.t == T_INT ? d.u.i == 0 : !(d.t == T_FLOAT && d.u.d != 0.0))
                    { rej = "DIV by zero ct"; goto out; }
            }
            break;
        case OP_CONCAT:
            /* P0-14：任意形态放行（helper 快慢路 + raise→deopt 已覆盖）。
             * 历史 LOOP 多连崩 = deps/ir call 实参装载乱序（MEM/CONST->
             * REG 装载晚于 REG->REG 洗牌，dst==src 同值实参读到未物化
             * 寄存器），已在 ir_emit_{x86,aarch64}(.dasc+生成头) 三 pass
             * 调序修复，复现 tmp-s2 → tests/lang/test-concat-loop-001.php */
            break;
        case OP_SHL: case OP_SHR:
            if (op->t2 != OPD_CONST || fn->lits[op->a2].t != T_INT)
                { rej = "SHL/SHR non-int ct"; goto out; }
            break;
        case OP_JMP: case OP_JMPZ: case OP_JMPNZ:
        case OP_RETURN:
            break;
        case OP_INIT_FCALL: {
            TFunc *callee = (TFunc *)op->ic;
            if (!op->ic) { rej = "INIT_FCALL unresolved"; goto out; }
            if ((uintptr_t)op->ic & 1)
                break;   /* builtin (ic|1): folded into tphp_jit_bcall (P0-1) */
            if (callee != fn && !jit_eligible_inner(callee, depth + 1))
                { rej = "callee ineligible"; goto out; }
            break;
        }
        case OP_SEND_VAL: case OP_SEND_VAR:
            if (op->t1 == OPD_CONST &&
                !lit_scalar(fn->lits[op->a1]) && fn->lits[op->a1].t != T_STR)
                { rej = "SEND const lit"; goto out; }
            break;
        case OP_ARRAY_APPEND:
            /* array literal 元素（keyed/append 双形态）：base 必须是槽，
             * key/value 槽位或字面量 */
            if (op->t1 == OPD_CONST || op->t1 == OPD_UNUSED) { rej = "ARRAY_APPEND base/形态"; goto out; }
            if (op->t2 == OPD_CONST &&
                !lit_scalar(fn->lits[op->a2]) && fn->lits[op->a2].t != T_STR)
                { rej = "ARRAY_APPEND key lit"; goto out; }
            if (op->tr == OPD_CONST &&
                !lit_scalar(fn->lits[op->ar]) && fn->lits[op->ar].t != T_STR)
                { rej = "ARRAY_APPEND val lit"; goto out; }
            break;
        case OP_DO_FCALL:
            break;
        case OP_INIT_NEW: case OP_NEW_USER: case OP_INIT_MCALL:
            /* P0-14 批2：对象链——调用序回放（vm.c tphp_jit_opseq）；
             * 嵌套/动态方法名/NEW_EX-LATE-DYN 等形态在 emit 侧 fail
             * （= 不编译，回退解释器，语义安全） */
            break;
        case OP_UNSET_CV:
            break;
        case OP_FETCH_PROP: case OP_ASSIGN_PROP:
        case OP_ISSET_PROP: case OP_UNSET_PROP:
            /* P0-14 批2-2：prop 单 op pad 执行（emit 见 lower_replay） */
            break;
        case OP_FE_RESET: case OP_FE_FETCH:
            /* P0-14 批3：foreach 单 op pad 执行（emit 见 lower_replay）——
             * 真 handler 读自身操作数写帧槽（itmp/ctmp 游标/val/key/htmp
             * 全在帧槽，跨迭代回放保持），raise 走 opseq deopt 门 */
            break;
        case OP_ISSET_DIM:
            /* P0-14 批4：isset($a[$k]) 单 op pad 执行——真 handler 用 rd
             * raw read（UNDEF 安全，isset 语义本就静默）+ wtake，零 raise */
            break;
        case OP_INIT_SMCALL: {
            /* 批3-3：静态方法直调——编译期解析可判子集并缓存 op->ic
             * （emit/compile_callees 复用）；callee 需全白名单合格 */
            const char *srej = NULL;
            TFunc *sm = jit_smcallee(fn, op, &srej);
            if (!sm) { rej = srej; goto out; }
            op->ic = sm;
            if (sm != fn && !jit_eligible_inner(sm, depth + 1))
                { rej = "SMCALL callee ineligible"; goto out; }
            break;
        }
        default:
            rej = tphp_op_name(op->op);   /* 白名单外 opcode（主审计目标） */
            goto out;
        }
    }
    if (recv_seen != fn->n_params) { rej = "RECV count mismatch"; goto out; }
    if (!jit_cfg_ok(fn)) { rej = "cfg guard"; goto out; }   /* v1.1 codegen 临时守卫 */
    return true;
out:
    /* P0-14 审计：拒因分布（pc 级）——TPHP_JIT_DEBUG=1 时输出 */
    if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0]) {
        fprintf(stderr, "[jit] reject(%s): %s (n_ops=%u)\n",
                fn->name ? tstr_val(fn->name) : "?", rej ? rej : "?", fn->n_ops);
    }
    return false;
}

bool tphp_jit_eligible(TFunc *fn) {
    bool ok = jit_eligible_inner(fn, 0);
    static int dbg;
    if (!dbg) { const char *e = getenv("TPHP_JIT_DEBUG"); dbg = e && e[0] ? 2 : 1; }
    if (dbg == 2 && fn)
        fprintf(stderr, "[jit] eligible(%s)=%d n_ops=%u\n",
                fn->name ? tstr_val(fn->name) : "?", ok, fn->n_ops);
    return ok;
}

/* ================================================================
 * Lowering
 * ================================================================ */
typedef struct {
    ir_ctx *ctx;
    TFunc *fn;
    /* 刀3（11-05）槽级标量写者分析：NULL=未分析（TPHP_SLOTGATE=0 /
     * calloc 失败 → 全部写点保门 = HEAD 行为）；hot[s]=1 = 槽 s 可能持有
     * refcounted（写点保 rel_dst_gate），0 = 槽恒标量/UNDEF → 写前释放
     * 必空转 → 免发。分析见 jit_scan_slot_writers。 */
    uint8_t *slot_hot;
    ir_ref r_vm, r_slots, r_ret;
    /* loop stack（v1.1：支持嵌套 for/while，深度8） */
    enum { JIT_MAX_LOOPS = 8 };
    int loop_sp;
    uint32_t loop_hdr[JIT_MAX_LOOPS];
    ir_ref loop_ref[JIT_MAX_LOOPS];
    int loop_cond[JIT_MAX_LOOPS];   /* 1 = 等待头部的第一个 JMPZ（循环条件） */
    /* P0-14 批5: if 内 continue 的 pending 回边——分支 ir_END() 暂存，
     * 体尾 trap_lb 处 MERGE_N(+体尾) 后统一 LOOP_END（LOOP_BEGIN 保持
     * 单回边 pos2，零 deps/ir 改动） */
    enum { JIT_LB_MAX = 16 };
    ir_ref lb_pend[JIT_MAX_LOOPS][JIT_LB_MAX];
    int lb_np[JIT_MAX_LOOPS];
    /* P0-17③ 批B'：折叠 fast 线的前向汇合——packed 直取分支内直出
     * FETCH_DIM_R+ADD 两 op 效果（零 TMP 槽往返）后，fast 线 END 暂存于
     * fwd_end，emit_seq 主循环推进到 fwd_pc 时与主线 END 做 MERGE_2 汇合。
     * 单 pending：嵌套折叠形态保守拒编。 */
    ir_ref fwd_end;
    uint32_t fwd_pc;
    uint8_t fwd_active;
    uint8_t fwd_is_acc;     /* 当前 fwd 挂起属于 acc 折叠（phion）——
                             * fwd merge 据此决定是否构建 acc_iter（防
                             * 非 RMW 折叠污染 acc 回边值） */
    ir_ref fwd_pay;         /* 批C slice-1: fast 线产出的 acc pay（fwd merge PHI 输入） */
    /* P0-17③ 批C slice-1：acc（累加器 CV）loop-carried pay 寄存器化——
     * canonical 形态守卫（fold_acc_scan）通过后，折叠 fast 线对 $s 只写
     * pay（tag 恒 entry 值、fast 线不触碰），$s pay 经 loop phi 跨迭代
     * 驻寄存器：fast 读 = phi（零槽 load），回边值 = fwd merge 的
     * reconcile PHI（fast ssum / slow 槽 load 合一，零回边 load）。
     * deopt/exit 安全性：pay 每迭代 store、tag 不被 fast 线触碰 → slot
     * 恒 current，无需 spill 基建（完全去 store 的 spill 基建=后续批）。
     * acc_on 激活时循环体无嵌套结构（守卫保证）→ 内层 pop 不会清掉
     * 外层激活态。 */
    ir_ref acc_phi;         /* loop phi（IR_I64 pay），0 = 未激活 */
    ir_ref acc_iter;        /* fwd merge reconcile phi（回边喂入口） */
    uint32_t acc_slot;      /* acc 槽 idx */
    uint8_t acc_on;         /* 本层循环 acc 跟踪激活 */
    /* POC array-read preguard（memory/2026-11-04 §1 路线甲）：每层循环
     * 的 FETCH base 不变量外提。循环头 push 时 preguard_scan 静态证明
     * 体内（hdr..唯一回边）对 base 槽零 store、零数组变更（纯 op 白名单
     * + 写点扫描；tphp 无引用类型 → 无别名逃逸）→ 在循环前驱块一次求值
     * inv(tag==T_ARR)/payload/packed/n/data，体内 FETCH 只留 key 残余
     * （$i 经 INC 可溢出变 FLOAT → key tag/ULT 不可外提），慢路 v1
     * helper 照旧完整兜底。证明失败 → 该层 0 候选 = 原逐迭代装载，
     * 语义零漂移。 */
    enum { JIT_PRE_MAX = 4 };
    int pre_n[JIT_MAX_LOOPS];          /* 本层候选数（头 push 时重置） */
    uint32_t pre_slot[JIT_MAX_LOOPS][JIT_PRE_MAX];
    ir_ref pre_inv[JIT_MAX_LOOPS][JIT_PRE_MAX];  /* tag==T_ARR（前驱块求值，非 phi） */
    ir_ref pre_a[JIT_MAX_LOOPS][JIT_PRE_MAX];    /* TArr* payload phi（IR_I64） */
    ir_ref pre_pk[JIT_MAX_LOOPS][JIT_PRE_MAX];   /* packed U8 phi */
    ir_ref pre_n64[JIT_MAX_LOOPS][JIT_PRE_MAX];  /* (u64)n phi */
    ir_ref pre_dt[JIT_MAX_LOOPS][JIT_PRE_MAX];   /* data 指针 phi（IR_ADDR） */
    /* 类型特化批1（PLAN/type-spec.md §5，TPHP_TYPE_SPEC）：preheader
     * guard 通过后的特化上下文。guard emit 于循环 push 处（镜像
     * OP_SEND_CAP：IF(!g) TRUE 短块 ir_RETURN(0)=fn 级 pc0 解释器重启，
     * FALSE 线继续 emit 循环）。作用域 [spec_lo,spec_hi]=[hdr,be]，
     * emit_seq 主循环按 pc 出体自动失效。作用域内检查点按身份跳过
     * tag 检查：acc/$i 的恒 INT 由 guard + 溢出 RETURN(0) 保证，
     * 元素恒 INT 由 g3 全元素扫描 + 体内零数组写白名单保证。 */
    uint8_t spec;              /* 1 = 当前 emit 在特化循环体内 */
    uint8_t spec_tmp_valid;    /* FETCH 尾置位：紧邻 ADD 的 src 即元素 TMP */
    uint32_t spec_lo, spec_hi; /* 作用域 [hdr, be] */
    uint32_t spec_acc;         /* 累加器 CV（$s） */
    uint32_t spec_idx;         /* 索引 CV（$i = FETCH key 槽 = INC a1） */
    uint32_t spec_tmp;         /* FETCH dst（元素值 TMP，ADD tb 消费用） */
    uint32_t spec_body_tmp;    /* 刀15：体内 FETCH dst 恒等身份（spec_scan
                                * 输出）。非零 ⟺ preheader 已 gate+UNDEF
                                * 消灭该槽 ⟺ dimic_take 可免 release。
                                * 不受 FETCH 尾 spec_tmp 覆盖影响。 */
    uint8_t spec_fwd_valid;    /* 刀16：FETCH 值直达挂起——pay 经 MERGE phi
                                * 桥出，紧邻 ADD 免槽往返（dst 槽不写）。 */
    uint8_t spec_fwd_kind;     /* 直达目标操作数身份（FETCH dst 的 kind，
                                * 消费判别用：kind+idx 双等才算） */
    uint32_t spec_fwd_idx;
    ir_ref spec_fwd_pay;       /* phi 结果（hit=IF_TRUE 内 element load /
                                * miss=helper 写 dst 后 load） */
    uint8_t spec_kfwd_valid;   /* 批4b-i key-fwd：F1 (pay,tag) phi 已挂起，
                                * 供紧邻 F2 的键位（消费即清） */
    uint32_t spec_kfwd_idx;    /* 挂起键 phi 的目标槽（F1 dst——F2 按
                                * kind+idx 双等消费，镜像 spec_fwd 纪律） */
    ir_ref spec_kfwd_pay;      /* F1 取值 pay phi（三层 MERGE 汇出） */
    ir_ref spec_kfwd_tag;      /* F1 取值 tag phi——F2 STR 分派/探针比对用 */
    uint8_t spec_chain;        /* 批4b-ii（链式 fwd）：scan 放行 N-fetch 链
                                 * 加形态——值走 spec_ctab 双通道（本字段与
                                 * spec_fwd/kfwd 通道由 scan 模式分叉互斥） */
    struct {
        uint8_t valid, kind, unwritten;
        uint32_t idx;
        ir_ref pay;            /* phi/sum 直达值；0 = 槽已写，消费端 load */
    } spec_ctab[2];            /* 值待发表：至多 2 同时在场（F1 活跃跨 F2 =
                                 * 2；此后恒 {前链 dst, 本 fetch} = 2，插入
                                 * 时至多 1 ⇒ 必有空位）。消费按 kind+idx——
                                 * 消费点两操作数恒同时活跃（活区间重叠 ⇒ 槽
                                 * 异、tmp_new 单调 ⇒ idx 异）无歧义 */
    ir_ref spec_ctab_sum;      /* lower_binop 链加尾记录的 sum ref（dispatch
                                 * 挂链尾用；慢线路径恒 0 = 不携带） */
    uint8_t spec_kstr;         /* 批4c（单 fetch ht 化）：0=非本形态（read/
                                 * kv/chain 恒 0）、1=CV 键（guard 合取键槽
                                 * tag==STR）、2=STR 字面量键（编译期已证，
                                 * guard 免查）——非 0 ⟺ FETCH 走免 STR
                                 * 分派直探（g.kt 预置 CONST T_STR） */
    uint32_t spec_base;        /* 批2：guard 证过的 base CV（FETCH 结构折叠
                                * 只对本 base 生效——cond 区异 base 的
                                * preguard 候选无 guard 证，不得折叠） */
    uint8_t spec_ht;           /* 批4a（§8.3）：本作用域 guard 走 ht 形态
                                * （tphp_jit_spec_guard_ht：hash 基+全活跃
                                * 桶值 INT）——spec_c（packed 结构证）须
                                * 排除本态；键 TMP 的 INT 证在本态无效
                                * （键来自 F1 取值=STR，spec_tmp 在 F1 尾
                                * 置位的窗口为假证，由相邻约束+本闸兜住） */
    int lb_jmpnz[JIT_MAX_LOOPS];  /* 批5挡板: 本层循环体内 emit 过 JMPNZ(||)
                                   * ——||结果喂 continue 的 pending 与 MERGE_N
                                   * 组合在 O1 下值错（ms1/ms2/ms5 最小复现，
                                   * join/dump 全对仍错 = codegen 域）→ push
                                   * 点拒编回退；||喂普通 if（logstat 形态）
                                   * 实测正确不受影响 */
    int fail;
    /* folded call in progress (INIT_FCALL/INIT_DYN … DO_FCALL) */
    TFunc *p_callee;
    void *p_bfn;                   /* builtin target (ic&1 form), P0-1 */
    uint32_t p_argc, p_arg_slot[9];
    int fail_line;
    uint32_t cur_pc;
    int p_dyn;                  /* 1 = INIT_DYN 动态调用 */
    uint32_t p_dyn_slot;        /* 闭包所在 CV 槽 */
    uintptr_t p_dyn_ic;         /* P0-15 批2: 该 INIT_DYN op 的 &op->ic（per-site
                                 * fn 快照槽；1=未解析哨兵，链尾存真 fn） */
    int p_sm;                   /* 批3-3: p_callee 来自 INIT_SMCALL（静态方法，
                                 * 帧布局 poff=1：slot0 保留/参数 1..） */
    /* 刀5C（11-06）：参数位置嵌套调用的 opseq 回放态。外层折叠序
     * （p_callee/p_bfn）进行中遇到内层 INIT_FCALL 时不再整函数 fail，
     * 而是把内层 INIT..DO 交给真 handler 回放（lower_replay），结果写
     * 回内层 dst 槽供外层 SEND 使用。单层兄弟嵌套可多次；多层嵌套
     * 与 OSR 态保守拒编（见 INIT_FCALL case 的保守门）。 */
    uint8_t nest_active;
    uint32_t nest_start;
    int p_mcall;                /* 批2-4: p_callee 来自 INIT_MCALL 折叠直调
                                 * （poff=1 + slot0=$this；OSR 守卫在 INIT
                                 * 失配 → OSR_SEQ_PC 整序回解释器，批2-7） */
    TClass *p_mcall_cls;        /* 批2-7: ic->decl——函数级守卫3（精确单态
                                 * cls 比对）在 lower_call_finish 用；sm 不
                                 * 跨 emit 保留故存 decl */
    int p_new;                  /* 批2-5: new 序折叠（无 ctor 直落——alloc+
                                 * dst，零守卫零 setjmp；SEND 跳过、实参仅
                                 * 求值副作用，不进 args 栈） */
    TClass *p_new_cls;          /* 批2-5: 折叠固化的 TClass*（ic 快照） */
    /* P0-14 批2: 调用序回放（INIT_NEW…NEW_USER / INIT_MCALL…DO_FCALL）。
     * 1 = new 序，2 = mcall 序，值即 p_seq_pc 起点 op 下标 */
    int p_seq;
    uint32_t p_seq_pc;
    /* 批2-7: 函数级 mcall 守卫失配收集——函数级 RETURN 0 = op_do_fcall
     * 从 pc 0 重启整函数（序前副作用双执行），故失配不 RETURN：END 暂存
     * 于此，OP_DO_FCALL 处 MERGE_N 聚合后走 lower_replay 只重跑调用序
     * （实参表达式已由 IR 求值，opseq 跳过表达式 op 恰正确），再与折叠
     * 成功路 MERGE 继续主序。单 site 上限 3 守卫 + entry + 9 契约 = 13。 */
    ir_ref mfail[16];
    int mfail_n;
    /* pre-allocated call scratch (entry-block ALLOCA — loop-body ALLOCA
     * stacks up under O1 and overflows after ~5k iterations) */
    ir_ref r_call_args;         /* argv buffer, 9 * TVAL_SZ */
    ir_ref r_call_slots;        /* P0-15 B dyn direct-call slots, 64 * TVAL_SZ */
    ir_ref r_call_clo;          /* closure TVal scratch */
    ir_ref r_call_ret;          /* return TVal scratch */
    /* P1-12 批1（PLAN/osr.md）：1 = 循环 OSR 模式——所有 fail/deopt 点的
     * ir_RETURN 值 = 当前 op 的解释器 pc（slots 同布局，解释器从该 pc 重跑
     * 该 op），函数级 = 0（deopt 语义由 op_do_fcall 处理）。见 OSR_RET_PC。 */
    int osr;
} JitC;

/* ir_builder macros expand to `ctx` — every lowering fn binds it locally */
#define JIT_CTX() ir_ctx *ctx = j->ctx; (void)ctx

/* 类型特化批1：该操作数位的 tag 已证恒 INT（spec 上下文）——acc/idx 由
 * preheader guard 证、spec_tmp 由全元素 INT 扫描 + FETCH 紧邻 ADD 时效证。
 * 恒 INT 的循环期持续性：acc/idx 只被配对 ADD/INC 写（spec_scan 写点
 * 约束），其溢出分支在 spec 下发 RETURN(0)（绝不落 FLOAT store）。
 * 置于此处因 lower_binop/lower_incdec（下方）先行引用。 */
static int spec_bit_int(JitC *j, uint8_t kind, uint32_t a) {
    if (!j->spec) return 0;
    if (kind == OPD_CV && (a == j->spec_acc || a == j->spec_idx)) return 1;
    if (kind != OPD_CONST && j->spec_tmp_valid && a == j->spec_tmp) return 1;
    return 0;
}

/* P1-12 批1（PLAN/osr.md）：fail/deopt 点的 ir_RETURN 值。OSR 模式返回
 * 当前 op 的解释器 pc——slots 同布局，解释器从该 pc 重跑该 op 即可（该
 * op 未完成语义，helper 返回 0 = 未写 dst，幂等）；函数级 = 0（deopt，
 * op_do_fcall 重跑语义不变）。 */
#define OSR_RET_PC(j) ((j)->osr ? (int32_t)(j)->cur_pc : 0)
/* 折叠 CALL 序（INIT_FCALL..DO_FCALL）的 deopt resume：OSR 下必须回到
 * 序起点（j->p_seq_pc）——INIT 之后 SEND 靠 args 栈、DO 靠 rec，单独重跑
 * DO 语义断裂；从起点重跑整序幂等（SEND_VAL/SEND_VAR 源恒为槽，args
 * 表达式在 seq 之外求值，不在重跑范围）。函数级 = 0（整函数重跑不变）。 */
#define OSR_SEQ_PC(j) ((j)->osr ? (int32_t)(j)->p_seq_pc : 0)

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

/* P0-12：slow 调用带 deopt 分支（raise → helper 返 0 → IR RETURN 0 传播）。
 * 分支留开：调用方后续 END/续接自然落在 IF_FALSE 内（bcall 同款结构）。 */
static void call_slow_binop(JitC *j, int opi, ir_ref da,
                            ir_ref ta, ir_ref pa, ir_ref tb, ir_ref pb) {
    JIT_CTX();
    ir_ref args[7] = { j->r_vm, ir_CONST_I32(opi), da, ta, pa, tb, pb };
    ir_ref ok = ir_CALL_N(IR_I32, ir_CONST_ADDR((uintptr_t)tphp_jit_binop_slow), 7, args);
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
    ir_IF_FALSE(ifd);
}
static void call_slow_cmp(JitC *j, int opi, ir_ref da,
                          ir_ref ta, ir_ref pa, ir_ref tb, ir_ref pb) {
    JIT_CTX();
    ir_ref args[7] = { j->r_vm, ir_CONST_I32(opi), da, ta, pa, tb, pb };
    ir_ref ok = ir_CALL_N(IR_I32, ir_CONST_ADDR((uintptr_t)tphp_jit_cmp_slow), 7, args);
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
    ir_IF_FALSE(ifd);
}
/* CAST 慢路调用（刀2）：helper 返 0（raise 已设）→ deopt RETURN；否则留
 * 在 IF_FALSE 继续——调用方以 END 收 ELSE 线再 MERGE（binop 同款姿势）。
 * 慢体自释放旧 dst → 发射器绝不在该分支前置 rel_dst_gate（防双释放）。 */
static void call_slow_cast(JitC *j, int ct, ir_ref da,
                           ir_ref ta, ir_ref pa) {
    JIT_CTX();
    ir_ref args[5] = { j->r_vm, ir_CONST_I32(ct), da, ta, pa };
    ir_ref ok = ir_CALL_N(IR_I32,
                          ir_CONST_ADDR((uintptr_t)tphp_jit_cast_slow), 5, args);
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
    ir_IF_FALSE(ifd);
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
    /* P0-cond（11-04 实测分歧修复）：对齐 tval_to_bool 的 STR/ARR 分支——
     * 旧链缺此两支，JIT 下 if("")/if([]) 恒真而解释器为假（探针实锤）。
     * tag 键控菱形：只在自身 tag 为真时解引用 payload，零跨类型解引用；
     * 假分支 phi 喂 false（恒被掩蔽）。UNDEF 不在此收口：op_bool_not 对
     * UNDEF 走 to_bool 默认(true)，仅 JMP位显式 falsy——由调用方
     * cond_truthy 加门，保各位语义与解释器各自一致。 */
    ir_ref is_s = ir_EQ(tag, ir_CONST_I32(T_STR));
    ir_ref ifs = ir_IF(is_s);
    ir_IF_TRUE(ifs);
        ir_ref len = ir_LOAD_U32(ir_ADD_A(pay, ir_CONST_ADDR(offsetof(TStr, len))));
        ir_ref c0 = ir_LOAD_U8(ir_ADD_A(pay, ir_CONST_ADDR(offsetof(TStr, c))));
        ir_ref sf = ir_OR(IR_BOOL, ir_EQ(len, ir_CONST_U32(0)),
                   ir_AND(IR_BOOL, ir_EQ(len, ir_CONST_U32(1)),
                          ir_EQ(c0, ir_CONST_U8('0'))));
        ir_ref eS = ir_END();
    ir_IF_FALSE(ifs);
        ir_ref eSf = ir_END();
    ir_MERGE_2(eS, eSf);
    ir_ref strf = ir_PHI_2(IR_BOOL, sf, ir_CONST_BOOL(false));
    ir_ref is_a = ir_EQ(tag, ir_CONST_I32(T_ARR));
    ir_ref ifa = ir_IF(is_a);
    ir_IF_TRUE(ifa);
        ir_ref n = ir_LOAD_U32(ir_ADD_A(pay, ir_CONST_ADDR(offsetof(TArr, n))));
        ir_ref af = ir_EQ(n, ir_CONST_U32(0));
        ir_ref eA = ir_END();
    ir_IF_FALSE(ifa);
        ir_ref eAf = ir_END();
    ir_MERGE_2(eA, eAf);
    ir_ref arrf = ir_PHI_2(IR_BOOL, af, ir_CONST_BOOL(false));
    ir_ref falsy3 = ir_OR(IR_BOOL, falsy2, ir_OR(IR_BOOL, strf, arrf));
    return ir_NOT_B(falsy3);
}
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

/* 刀2（11-04 释放门）：JIT 快线裸 store 覆盖旧 dst 不释放——解释器所有
 * 写值 op 都走 wtake（先 release 旧值再写），快线漏释即泄漏族
 * （tools/probe_release_family.php 实证 add/cmp/not/cast/const/incsuf/
 * call 七类 JIT on 0/300、off 300/300）。本门 = 写前释放旧 dst：
 * - 标量/UNDEF（tag<T_STR 或 >T_CLOSURE）跳过；≥T_STR 走 tval_release
 *   （interned/rc 递减/析构全在其内，与 wtake 同一函数）；
 * - 慢路分支不发（slow helper 内 compute 后自释放——前置门会双释放）；
 * - emit 判别 dst==源槽且该分支源必为标量（both_int/标量叶/fold other
 *   ==ar）→ 免发：分支条件即证明 dst 为标量，门恒空转。
 * 安全先例：IR 内 CALL tval_release 无 setjmp 已有三处（UNSET_CV、
 * dimic_take rc==1 支、mcall $this 归还）；块结构与 dimic_take release
 * 段同款（IF+END/MERGE_2）。 */
static void rel_dst_gate(JitC *j, ir_ref da) {
    JIT_CTX();
    ir_ref td = load_tag(j, da);
    ir_ref ifd = ir_IF(ir_AND(IR_BOOL,
        ir_GE(td, ir_CONST_I32(T_STR)), ir_LE(td, ir_CONST_I32(T_CLOSURE))));
    ir_IF_TRUE(ifd);
        ir_ref tr[1] = { da };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, tr);
        ir_ref eR = ir_END();
    ir_IF_FALSE(ifd);
        ir_ref eS = ir_END();
    ir_MERGE_2(eR, eS);
}

/* 刀3（11-05 槽级写者分析回收门税）：dst 槽恒标量/UNDEF → 免发释放门
 * （门对标量本就空转，省下的 = load_tag+区间比+IF/CALL/MERGE 静态布局
 * 税 = 刀2 量化的门税）。kind 必查（CONST lit 下标与槽下标撞数）；
 * 未分析/未知 kind → 保守保门。错判方向安全：漏降级只会跳过释放=泄漏
 * （probe_release_family 300/300 拦截），不会双释放——反向（多保门）
 * 即 HEAD 行为。soundness 细则全在 jit_scan_slot_writers 注。 */
static bool dst_gate_needed(JitC *j, TOp *op) {
    if (!j->slot_hot) return 1;
    if (op->tr != OPD_CV && op->tr != OPD_TMP) return 1;
    return j->slot_hot[op->ar];
}

/* ---- P0-13 v2：FETCH_DIM_R 命中路径全内联（JSC Baseline IC 形态）----
 * 两条互斥快路：packed int 直取（array-read 主形态）/ DimIC hash 快照
 * （kv-read 主形态）。IC 链序照抄 tarr_get_ic：arr→buckets→bpending→
 * bp→val_off→key tag→key pay——buckets(ic->bk) 比较先于 bp 解引用，是
 * 跨线程撕裂写序（arr 先写）下垃圾 bp 的安全门；v1 的语义论证照搬：
 * 链全锚定 live 数组状态，miss 兜底 helper（norm/raise/装填/absent 防
 * 陈旧全在其中），raw key≠norm 时链必 miss→helper——内联只赚“预规范化
 * 形态成立”的热路径。结构 = 逐层 IF + END + MERGE_2（lower_binop 既
 * 有姿势；LOOP 内 diamond 已由现有 binop/deopt 路径证明可用）。 */
typedef struct DimIcCtx {
    JitC *j;
    ir_ref dst, base, key, ic, a, kt, kp;
    int thin;   /* 批4a：链叶 take 瘦身透传（spec_ht+body_tmp 恒等双证；
                 * 非 spec 点恒 0——g 在 DimIcCtx 初始化处清零） */
    int fwd;    /* 批4a-ii：ht 直探值汇流开关（hit=LOAD pay 免 16B store、
                 * miss=helper 写 dst 后 load）——仅 emit_ht_fetch 填 */
    int kfwd_prod; /* 批4b-i F1 角色：phi 生产——链尾从 dst 收集 pay/tag
                    * （命中线免 take、miss 线 helper 已写 dst） */
    int kfwd_mat;  /* 批4b-i F2 角色：dimic_miss 前把 phi 键物化进 key 槽 */
    ir_ref kfwd_pay, kfwd_tag; /* F1=链尾收集输出；F2=待物化的 phi 键 */
    int kt_fix;    /* 批4c：键类型编译期/guard 已证恒 T_STR——emit_ht_fetch
                    * 跳过槽 tag 装载与整个 STR 分派菱形（g.kt 预置
                    * CONST T_STR；non-STR 线不可达不发） */
} DimIcCtx;

/* 批4b-i：kfwd 键物化（冷路径专用——F2 的 miss/deopt 出口前把 phi 键按
 * take 语义写回 g->key 槽）。rc 论证（§8.4）：
 * - 热路径键只被探针身份比较（tag/pay 双 EQ）、hash 读 + 惰性写 hash 字段
 *   （非所有权操作）、STR 分派比较——不落槽不逃逸，kb 元素由 kb 自身持有
 *   （spec_scan 写点约束证体内零写 kb）→ 免 addref 安全；
 * - 本函数只在冷出口跑：addref 新（先）→ release 旧槽（后）→ store
 *   （pay 先、tag 后），顺序镜像 dimic_take 防同对象先减归零 → 槽恒持
 *   一份 owned 引用：解释器从 F2 pc 复跑读键槽拿到 owned 值、帧销毁释放
 *   配平；物化必先 release 旧 → 反复物化不叠 rc、UNDEF 旧值 no-op。 */
static void kfwd_materialize(DimIcCtx *g) {
    JitC *j = g->j;
    JIT_CTX();
    /* --- addref 新（kfwd pay/tag 是 phi 值，无 vaddr，菱形按值判） --- */
    ir_ref ifv = ir_IF(ir_AND(IR_BOOL,
        ir_GE(g->kfwd_tag, ir_CONST_I32(T_STR)),
        ir_LE(g->kfwd_tag, ir_CONST_I32(T_CLOSURE))));
    ir_IF_TRUE(ifv);
        ir_ref gcv = ir_BITCAST_A(g->kfwd_pay);
        ir_ref fv = ir_ZEXT(IR_I32,
            ir_LOAD_U8(ir_ADD_A(gcv, ir_CONST_ADDR(offsetof(GCHead, flags)))));
        ir_ref ifiv = ir_IF(ir_EQ(ir_AND_I32(fv, ir_CONST_I32(GC_FLAG_INTERNED)),
                                  ir_CONST_I32(0)));
        ir_IF_TRUE(ifiv);
            ir_ref rcv = ir_LOAD_I32(gcv);   /* rc @0 */
            ir_STORE(gcv, ir_ADD_I32(rcv, ir_CONST_I32(1)));
            ir_ref eV1 = ir_END();
        ir_IF_FALSE(ifiv);
            ir_ref eV0 = ir_END();
        ir_MERGE_2(eV1, eV0);
        ir_ref eRefV = ir_END();
    ir_IF_FALSE(ifv);
        ir_ref eScV = ir_END();
    ir_MERGE_2(eRefV, eScV);

    /* --- release 旧槽（dimic_take release dst 同款，源换 g->key） --- */
    ir_ref td = ir_LOAD_I32(ir_ADD_A(g->key, ir_CONST_ADDR(TVAL_TAG)));
    ir_ref ifd = ir_IF(ir_AND(IR_BOOL,
        ir_GE(td, ir_CONST_I32(T_STR)), ir_LE(td, ir_CONST_I32(T_CLOSURE))));
    ir_IF_TRUE(ifd);
        ir_ref gcd = ir_LOAD_A(g->key);
        ir_ref fd = ir_ZEXT(IR_I32,
            ir_LOAD_U8(ir_ADD_A(gcd, ir_CONST_ADDR(offsetof(GCHead, flags)))));
        ir_ref ifid = ir_IF(ir_EQ(ir_AND_I32(fd, ir_CONST_I32(GC_FLAG_INTERNED)),
                                  ir_CONST_I32(0)));
        ir_IF_TRUE(ifid);
            ir_ref rcd = ir_LOAD_I32(gcd);
            ir_ref ifrc = ir_IF(ir_GT(rcd, ir_CONST_I32(1)));
            ir_IF_TRUE(ifrc);
                ir_STORE(gcd, ir_SUB_I32(rcd, ir_CONST_I32(1)));
                ir_ref eDec = ir_END();
            ir_IF_FALSE(ifrc);
                ir_ref tr[1] = { g->key };
                ir_CALL_N(IR_VOID,
                          ir_CONST_ADDR((uintptr_t)tval_release), 1, tr);
                ir_ref eSlow = ir_END();
            ir_MERGE_2(eDec, eSlow);
            ir_ref eD1 = ir_END();
        ir_IF_FALSE(ifid);
            ir_ref eD0 = ir_END();
        ir_MERGE_2(eD1, eD0);
        ir_ref eRefD = ir_END();
    ir_IF_FALSE(ifd);
        ir_ref eScD = ir_END();
    ir_MERGE_2(eRefD, eScD);

    /* --- 16B store（pay 先、tag 后，与 dimic_take 同序） --- */
    ir_STORE(g->key, g->kfwd_pay);
    ir_STORE(ir_ADD_A(g->key, ir_CONST_ADDR(TVAL_TAG)), g->kfwd_tag);
}

/* 批4b-ii（链式 fwd）：值待发表操作。put = FETCH 尾/链加尾挂值；take =
 * lower_binop 按 kind+idx 消费即清（顺手对齐清 spec_tmp_valid 防串，镜像
 * 1839 消费即清纪律）；pay=0 表示槽已写（fwd 门失败/纯证明模式），消费
 * 端 load 照旧且 INT 证明仍由条目给出。 */
static void ctab_put(JitC *j, uint8_t kind, uint32_t idx, ir_ref pay, int unwritten) {
    for (int i = 0; i < 2; i++) {
        if (!j->spec_ctab[i].valid) {
            j->spec_ctab[i].valid = 1;
            j->spec_ctab[i].kind = kind;
            j->spec_ctab[i].idx = idx;
            j->spec_ctab[i].pay = pay;
            j->spec_ctab[i].unwritten = (uint8_t)unwritten;
            return;
        }
    }
    /* 不可达（结构证明：插位时至多 1 在场）——保守不挂，消费端退化为
     * 槽 load + 既有动态检查，正确性无虞 */
}
static int ctab_take(JitC *j, uint8_t kind, uint32_t a, ir_ref *pay, int *unw) {
    if (!j->spec_chain) return 0;
    for (int i = 0; i < 2; i++)
        if (j->spec_ctab[i].valid && j->spec_ctab[i].kind == kind &&
            j->spec_ctab[i].idx == a) {
            *pay = j->spec_ctab[i].pay;
            *unw = j->spec_ctab[i].unwritten;
            j->spec_ctab[i].valid = 0;
            if (j->spec_tmp_valid && j->spec_tmp == a) j->spec_tmp_valid = 0;
            return 1;
        }
    return 0;
}
static void ctab_clear(JitC *j) {
    j->spec_ctab[0].valid = j->spec_ctab[1].valid = 0;
    j->spec_ctab_sum = 0;
}
/* 物化：deopt/raise 出口前把 unwritten（phi 直达、槽未写）条目写回槽——
 * rel_dst_gate 自带释放门 ⇒ 槽持 UNDEF/INT/任意旧值皆安全（不依赖
 * preheader 消灭，TAKE_THIN=0 行为不变）；值恒 INT 标量、无 rc 义务，
 * 门内空转。链 dst 恒 store（unwritten=0）永不需要物化。 */
static void ctab_mat_all(JitC *j) {
    JIT_CTX();
    for (int i = 0; i < 2; i++) {
        if (j->spec_ctab[i].valid && j->spec_ctab[i].unwritten) {
            ir_ref sa = slot_addr(j, j->spec_ctab[i].idx);
            rel_dst_gate(j, sa);
            store_imm(j, sa, JT_INT, j->spec_ctab[i].pay);
        }
    }
}

/* miss：整链落 v1 helper + deopt（线留开供 MERGE；RETURN 线不参与）。
 * count=true = 进过 IC 链的失败——misses++（generic 回退门计数，锁死
 * 态与非数组/packed 越界形态不计）。 */
static void dimic_miss(DimIcCtx *g, bool count) {
    JitC *j = g->j;
    JIT_CTX();
    if (g->kfwd_mat) kfwd_materialize(g);   /* 批4b-i：helper 读键槽前物化 */
    if (count) {
        ir_ref ma = ir_ADD_A(g->ic, ir_CONST_ADDR(offsetof(DimIC, misses)));
        ir_STORE(ma, ir_ADD_I32(ir_LOAD_I32(ma), ir_CONST_I32(1)));
    }
    ir_ref t[5] = { g->j->r_vm, g->dst, g->base, g->key, g->ic };
    ir_ref ok = ir_CALL_N(IR_I32,
                          ir_CONST_ADDR((uintptr_t)tphp_jit_fetch_dim_r), 5, t);
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        /* 批4b-ii：链式 deopt（helper 返 0 = 未写 dst，解释器从本 op 重跑
         * 后继续走体）前物化全部 phi 直达挂起值——本 helper 不读其它
         * fetch 槽（只写 g->dst），故成功线无需物化（后续站点自会兜）。 */
        if (j->spec_chain) ctab_mat_all(j);
        ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
    ir_IF_FALSE(ifd);
}

/* v3.1 take 全内联（镜像 tval_addref/tval_release 热路径，顺序严格与 C
 * 一致：**先 addref v 后 release dst**——v 与 dst 可能同对象，先增后减
 * 防归零）：
 *   addref: tag∈[T_STR,T_CLOSURE] && !(flags&INTERNED) → rc++；其余 skip
 *   release: tag∈[T_STR,CLOSURE] && !INTERNED && rc>1 → rc--（color DEAD
 *     与正常色 rc>1 都是 rc--，正交等价，无需查 color）；标量/interned/
 *     rc==1（= DEAD rc-- 或 switch destroy 慢路）→ call tval_release 原样
 *   misses 清零（叶子命中 = 形态稳定）随取值一并生成 */
/* P0-17③ take 消融结论（11-04，tools/ablate_take.php）：两门动态执行≈0
 * （INT 短路 4 指令），总税 0.070 ≈ array-read 25% 全在静态布局（冷段
 * 横跨热 fast→MERGE ~2-3 周期/迭代）。两个修复方案已实测否决：
 * - outline（两门移 C helper tphp_dimic_take_slow）：string 场景 CALL 税
 *   +18.8%（kv 0.1404→0.1668）、int 无收益；
 * - 双 INT 快路（IF 跳两门）：int 收益在噪声内（6 轮 0.307 vs 0.3117）、
 *   str 微退（0.142 vs 0.1404），guard 自身也付 EQ×2+AND+IF。
 * 结论：保 HEAD 内联形态；余下税 = 后端 block layout（~0.016）+ 形态
 * guard（~0.05，需类型反馈/形态证明），记 backlog 不在本刀动。 */
static void dimic_take(DimIcCtx *g, ir_ref vaddr, int thin) {
    /* 刀15 thin 版（type-spec spec_c 折叠路径专用）：三免 + 16B store。
     * - 免 misses 清零：spec site 的 generic 门批2 不 emit、miss 计数
     *   dimic_miss(false) 不计、C 侧仅 init（array.c:426）→ 该字段
     *   零读者零写者；
     * - 免 addref 菱形 + vtag load：元素恒 INT（guard g3 全元素扫描 +
     *   体内零数组写白名单）→ [STR,CLOSURE] 区间恒假，tag 直落
     *   CONST INT；
     * - 免 release dst 菱形：dst=体内 FETCH dst——preheader 已恒发
     *   gate+UNDEF 消灭槽遗留（spec_body_tmp 非零=已消灭），此后每
     *   迭代 dst=take 写入的元素值（恒 INT）；miss 路径 helper 取值
     *   成功同样写元素（恒 INT）、越界则 UndefinedError raise 不写
     *   dst（deopt 重跑，test-jit-takethin-001 b2 实证）——dst 全程
     *   恒标量。首迭代依据来自 preheader 一次性 gate，每调用一次摊销。
     * 语义等价：值写 dst（pay 先、tag 后）逐条与慢版一致，仅省去
     * 必空转的三门（11-04 消融"动态≈0、税在静态布局"的残余主体）。 */
    if (thin) {
        JitC *j = g->j;
        JIT_CTX();
        ir_STORE(g->dst, ir_LOAD_I64(vaddr));
        ir_STORE(ir_ADD_A(g->dst, ir_CONST_ADDR(TVAL_TAG)),
                 ir_CONST_I32(JT_INT));
        return;
    }
    JitC *j = g->j;
    JIT_CTX();
    ir_STORE(ir_ADD_A(g->ic, ir_CONST_ADDR(offsetof(DimIC, misses))),
             ir_CONST_I32(0));
    ir_ref vtag = ir_LOAD_I32(ir_ADD_A(vaddr, ir_CONST_ADDR(TVAL_TAG)));

    /* --- addref v --- */
    ir_ref ifv = ir_IF(ir_AND(IR_BOOL,
        ir_GE(vtag, ir_CONST_I32(T_STR)), ir_LE(vtag, ir_CONST_I32(T_CLOSURE))));
    ir_IF_TRUE(ifv);
        ir_ref gcv = ir_LOAD_A(vaddr);
        ir_ref fv = ir_ZEXT(IR_I32,
            ir_LOAD_U8(ir_ADD_A(gcv, ir_CONST_ADDR(offsetof(GCHead, flags)))));
        ir_ref ifiv = ir_IF(ir_EQ(ir_AND_I32(fv, ir_CONST_I32(GC_FLAG_INTERNED)),
                                  ir_CONST_I32(0)));
        ir_IF_TRUE(ifiv);
            ir_ref rcv = ir_LOAD_I32(gcv);   /* rc @0 */
            ir_STORE(gcv, ir_ADD_I32(rcv, ir_CONST_I32(1)));
            ir_ref eV1 = ir_END();
        ir_IF_FALSE(ifiv);
            ir_ref eV0 = ir_END();
        ir_MERGE_2(eV1, eV0);
        ir_ref eRefV = ir_END();
    ir_IF_FALSE(ifv);
        ir_ref eScV = ir_END();
    ir_MERGE_2(eRefV, eScV);

    /* --- release dst --- */
    ir_ref td = ir_LOAD_I32(ir_ADD_A(g->dst, ir_CONST_ADDR(TVAL_TAG)));
    ir_ref ifd = ir_IF(ir_AND(IR_BOOL,
        ir_GE(td, ir_CONST_I32(T_STR)), ir_LE(td, ir_CONST_I32(T_CLOSURE))));
    ir_IF_TRUE(ifd);
        ir_ref gcd = ir_LOAD_A(g->dst);
        ir_ref fd = ir_ZEXT(IR_I32,
            ir_LOAD_U8(ir_ADD_A(gcd, ir_CONST_ADDR(offsetof(GCHead, flags)))));
        ir_ref ifid = ir_IF(ir_EQ(ir_AND_I32(fd, ir_CONST_I32(GC_FLAG_INTERNED)),
                                  ir_CONST_I32(0)));
        ir_IF_TRUE(ifid);
            ir_ref rcd = ir_LOAD_I32(gcd);
            ir_ref ifrc = ir_IF(ir_GT(rcd, ir_CONST_I32(1)));
            ir_IF_TRUE(ifrc);
                ir_STORE(gcd, ir_SUB_I32(rcd, ir_CONST_I32(1)));
                ir_ref eDec = ir_END();
            ir_IF_FALSE(ifrc);
                /* rc==1（含 GC DEAD 特例）：慢路原样（destroy/释放链） */
                ir_ref tr[1] = { g->dst };
                ir_CALL_N(IR_VOID,
                          ir_CONST_ADDR((uintptr_t)tval_release), 1, tr);
                ir_ref eSlow = ir_END();
            ir_MERGE_2(eDec, eSlow);
            ir_ref eD1 = ir_END();
        ir_IF_FALSE(ifid);
            ir_ref eD0 = ir_END();   /* interned dst：C 同款 skip */
        ir_MERGE_2(eD1, eD0);
        ir_ref eRefD = ir_END();
    ir_IF_FALSE(ifd);
        ir_ref eScD = ir_END();
    ir_MERGE_2(eRefD, eScD);

    /* --- 16B store（pay 先、tag 后） --- */
    ir_STORE(g->dst, ir_LOAD_I64(vaddr));
    ir_STORE(ir_ADD_A(g->dst, ir_CONST_ADDR(TVAL_TAG)), vtag);
}

/* 刀6（11-06，kv 形态侦察批）：STR probe0 失败后的 sequential probe 回退。
 * 镜像 C 侧 tarr_get_ic ② hash 直达的 ≤4 步开放寻址（array.c:376-392；
 * IR 侧 probe0 已是 p=0，这里补 p=1..3）。kv 侦察实锤：8 键满表哈希
 * 冲突下 probe0 恒 miss → cases[1..7] 全死哨兵 → 每轮 helper CALL
 * （27% 调用率 ≈ +7.6ms = kv 差距 47%，memory/2026-11-05.md §kv 侦察）。
 * 语义安全：
 *  - 匹配 = 活桶（val_off != UINT32_MAX）+ key tag/pay 全身份（与
 *    probe0/case 链同款；interned 串指针相等即全身份，是 C 侧
 *    KEY_PTR_EQ||hash+key_eq 的保守子集，miss 一律有兜底）；
 *  - C 侧以 T_NULL 空桶终止探测链：IR 侧空桶 tag 与 STR key 不等自然
 *    不命中，继续探测最多 N 步后落 case 链（开放寻址保证空桶后无目标
 *    键、全身份比较杜绝误命中）；
 *  - 命中复用 dimic_take（与 case 链叶同款）；超步数/不命中一律落
 *    dimic_ic_chain(g,5) → helper → C 侧 arr_find 完整兜底，零语义漂移；
 *  - 不回填 ic->cases[0]（缓存非语义；C 侧 helper 命中时仍会回填），
 *    避免命中热路径多一次 store。
 * 开关 TPHP_IC_PROBE=0 关（同二进制 A/B，回到 probe0 直落 case 链）。 */
#define IC_PROBE_STEPS 3
static int ic_probe_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_IC_PROBE");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}
static ir_ref dimic_ic_chain(DimIcCtx *g, int lvl);

static ir_ref dimic_linear_probe(DimIcCtx *g, ir_ref slot, ir_ref hm,
                                 ir_ref bk, int p) {
    JitC *j = g->j;
    JIT_CTX();
    if (p > IC_PROBE_STEPS) return dimic_ic_chain(g, 5);
    ir_ref s = ir_AND(IR_U32, ir_ADD_U32(slot, ir_CONST_U32(1)), hm);
    ir_ref zs = ir_ZEXT(IR_U64, s);
    ir_ref bp = ir_ADD_A(ir_ADD_A(bk, ir_SHL_U64(zs, ir_CONST_U64(4))),
                         ir_SHL_U64(zs, ir_CONST_U64(3)));   /* *24 */
    ir_ref bkey = ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, key)));
    ir_ref m = ir_AND(IR_BOOL,
                ir_AND(IR_BOOL,
                       ir_EQ(g->kt, ir_LOAD_I32(
                           ir_ADD_A(bkey, ir_CONST_ADDR(TVAL_TAG)))),
                       ir_EQ(g->kp, ir_LOAD_I64(bkey))),
                ir_NE(ir_LOAD_U32(ir_ADD_A(bp,
                           ir_CONST_ADDR(offsetof(ABucket, val_off)))),
                      ir_CONST_U32(0xFFFFFFFFu)));
    ir_ref ifp = ir_IF(m);
    ir_IF_TRUE(ifp);
        ir_ref voff = ir_LOAD_U32(
            ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, val_off))));
        ir_ref data = ir_LOAD_A(
            ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, data))));
        dimic_take(g, ir_ADD_A(data,
                     ir_SHL_U64(ir_ZEXT(IR_U64, voff), ir_CONST_U64(4))), g->thin);
        ir_ref epT = ir_END();
    ir_IF_FALSE(ifp);
        ir_ref epF = dimic_linear_probe(g, s, hm, bk, p + 1);
    ir_MERGE_2(epT, epF);
    return ir_END();
}

/* v3 多态链（v3.2' hash 直选）：lvl 0..2 = 共享层（arr/buckets/bpending，
 * 失败 = 整链 miss 计数）；lvl 3 = cases[0] 单槽（单态 site 一步命中，
 * 与 v3.1 同构）；lvl 4 = STR 分派——TRUE 内 probe0 桶直取（镜像 C
 * tarr_get_ic 两级快路：h 惰性冷填 → &hmask → bp=ic->bk+slot*24 →
 * val_off/tag/pay 三比），命中取值、失败落 case 链兜底（轮转键不产生
 * miss 计数 = 锁死门不错杀）；非 STR 直接落 case 链（INT 零改动）；
 * lvl 5..4+N-1 = cases[1..N-1] 线性兜底，末 case F 才 miss 计数。
 * 结构仍是逐层 IF+END+MERGE_2。[kp+28] 读仅在 STR 门内（INT 键的 kp
 * 是整数值）。 */
static ir_ref dimic_ic_chain(DimIcCtx *g, int lvl) {
    JitC *j = g->j;
    JIT_CTX();
    if (lvl == 4) {
        /* STR 分派 + probe0（v3.2'）：非短路 AND 安全——bp 经 mask 恒在
         * 桶数组内无野读；tag+pay 指针相等即全身份（hash 比较省略，h 仅
         * 用于 slot 计算）。 */
        ir_ref ifstr = ir_IF(ir_EQ(g->kt, ir_CONST_I32(T_STR)));
        ir_IF_TRUE(ifstr);
            ir_ref ksa = ir_LOAD_A(g->key);   /* TStr*（key 槽 payload） */
            ir_ref h0 = ir_LOAD_U32(
                ir_ADD_A(ksa, ir_CONST_ADDR(offsetof(TStr, hash))));
            ir_ref ifh0 = ir_IF(ir_EQ(h0, ir_CONST_U32(0)));
            ir_IF_TRUE(ifh0);
                /* 冷：hash 惰性未填 → tstr_hash 计算并写回（C 三元同款；
                 * 返回值不用——合流后统一 reload） */
                ir_ref th[1] = { ksa };
                ir_CALL_N(IR_U32,
                          ir_CONST_ADDR((uintptr_t)tstr_hash), 1, th);
                ir_ref ehT = ir_END();
            ir_IF_FALSE(ifh0);
                ir_ref ehF = ir_END();
            ir_MERGE_2(ehT, ehF);
            /* 合流后统一 load：热路多一次冗余 load，免跨支 value-phi */
            ir_ref h = ir_LOAD_U32(
                ir_ADD_A(ksa, ir_CONST_ADDR(offsetof(TStr, hash))));
            ir_ref hm = ir_LOAD_U32(
                ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, hmask))));
            ir_ref slot = ir_AND(IR_U32, h, hm);   /* U32 域纪律 */
            ir_ref bk = ir_LOAD_A(
                ir_ADD_A(g->ic, ir_CONST_ADDR(offsetof(DimIC, bk))));
            ir_ref zs = ir_ZEXT(IR_U64, slot);
            ir_ref bp = ir_ADD_A(ir_ADD_A(bk, ir_SHL_U64(zs, ir_CONST_U64(4))),
                                 ir_SHL_U64(zs, ir_CONST_U64(3)));   /* *24 */
            ir_ref bkey = ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, key)));
            ir_ref m = ir_AND(IR_BOOL,
                        ir_AND(IR_BOOL,
                               ir_EQ(g->kt, ir_LOAD_I32(
                                   ir_ADD_A(bkey, ir_CONST_ADDR(TVAL_TAG)))),
                               ir_EQ(g->kp, ir_LOAD_I64(bkey))),
                        ir_NE(ir_LOAD_U32(ir_ADD_A(bp,
                                   ir_CONST_ADDR(offsetof(ABucket, val_off)))),
                              ir_CONST_U32(0xFFFFFFFFu)));
            ir_ref ifm = ir_IF(m);
            ir_IF_TRUE(ifm);
                ir_ref voff = ir_LOAD_U32(
                    ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, val_off))));
                ir_ref data = ir_LOAD_A(
                    ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, data))));
                dimic_take(g, ir_ADD_A(data,
                             ir_SHL_U64(ir_ZEXT(IR_U64, voff), ir_CONST_U64(4))), g->thin);
                ir_ref emT = ir_END();
            ir_IF_FALSE(ifm);
                /* 刀6：probe0 miss → sequential probe 回退（同二进制 A/B
                 * 开关；关 = v3.2' 原行为直落 case 链） */
                ir_ref emF = ic_probe_enabled()
                    ? dimic_linear_probe(g, slot, hm, bk, 1)
                    : dimic_ic_chain(g, 5);
            ir_MERGE_2(emT, emF);
            ir_ref eStr = ir_END();
        ir_IF_FALSE(ifstr);
            ir_ref eNo = dimic_ic_chain(g, 5);       /* 非 STR → 原链零改动 */
        ir_MERGE_2(eStr, eNo);
        return ir_END();
    }
    ir_ref bp = IR_UNUSED;
    ir_ref cond;
    switch (lvl) {
    case 0:
        cond = ir_EQ(ir_LOAD_A(ir_ADD_A(g->ic, ir_CONST_ADDR(offsetof(DimIC, arr)))), g->a);
        break;
    case 1:
        cond = ir_EQ(ir_LOAD_A(ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, buckets)))),
                     ir_LOAD_A(ir_ADD_A(g->ic, ir_CONST_ADDR(offsetof(DimIC, bk)))));
        break;
    case 2:
        cond = ir_EQ(ir_LOAD_U32(ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, bpending)))),
                     ir_CONST_U32(0));
        break;
    default: {
        int ci = lvl == 3 ? 0 : lvl - 4;   /* lvl3=case0；lvl5..=case1.. */
        bp = ir_LOAD_A(ir_ADD_A(g->ic,
              ir_CONST_ADDR(offsetof(DimIC, cases) + (uintptr_t)ci * sizeof(ABucket *))));
        ir_ref bkey = ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, key)));
        /* tag+pay 合并 key 同一性（norm 后 intern 串指针等 / int 值等），
         * val_off 非 tombstone——全部对 bp 安全解引用（哨兵保证） */
        cond = ir_AND(IR_BOOL,
                  ir_AND(IR_BOOL,
                         ir_EQ(g->kt, ir_LOAD_I32(ir_ADD_A(bkey, ir_CONST_ADDR(TVAL_TAG)))),
                         ir_EQ(g->kp, ir_LOAD_I64(bkey))),
                  ir_NE(ir_LOAD_U32(ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, val_off)))),
                        ir_CONST_U32(0xFFFFFFFFu)));
        break;
    }
    }
    bool leaf_here = (lvl >= 3);   /* case 条件通过即命中取值 */
    bool last = (lvl == 4 + DIM_IC_CASES - 1);   /* 末 case（lvl11 @ N=8） */
    ir_ref ifx = ir_IF(cond);
    ir_IF_TRUE(ifx);
        ir_ref ct;
        if (leaf_here) {
            ir_ref voff = ir_LOAD_U32(
                ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, val_off))));
            ir_ref data = ir_LOAD_A(ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, data))));
            dimic_take(g, ir_ADD_A(data,
                         ir_SHL_U64(ir_ZEXT(IR_U64, voff), ir_CONST_U64(4))), g->thin);
            ct = ir_END();
        } else {
            ct = dimic_ic_chain(g, lvl + 1);
        }
    ir_IF_FALSE(ifx);
        ir_ref cf;
        if (leaf_here) {
            if (last) {          /* 8 case 全败 = 整链失败 → 计数 */
                dimic_miss(g, true);
                cf = ir_END();
            } else {
                cf = dimic_ic_chain(g, lvl + 1);   /* 下一 case */
            }
        } else {
            dimic_miss(g, true);  /* 共享层失败 = 数组级失效 → 计数 */
            cf = ir_END();
        }
    ir_MERGE_2(ct, cf);
    /* 批4b-i：链尾 lift（仅 kv F1 的 kfwd 生产——g->kfwd_prod 门控，
     * 其余 fetch 恒 0 零 IR 变化）。链 END 封块后调用方无法 load，
     * 故在最外层（lvl==0）尾 MERGE 后、END 前从键槽收集 (pay,tag)：
     * 链叶 take 已写槽（F1 thin=0 full take），lift 读槽值真；槽内
     * owned 残留由物化 release-旧/帧销毁配平。 */
    if (g->kfwd_prod && lvl == 0) {
        g->kfwd_pay = load_pay(j, g->dst);
        g->kfwd_tag = load_tag(j, g->dst);
    }
    return ir_END();
}

/* ---- 批4a-ii（PLAN/type-spec.md §8.3-3/4）：spec_ht 直探 ----
 * 免 inv 外层（guard 已证 ARR）、IC 快照门（arr/bk/bpending）、case0、
 * case1-7 链与 generic 8-fail 门：guard_ht + 体内零写 ⇒ buckets/hmask/
 * bpending 循环期不变，直探直接读 g->a（无需 IC 快照）。结构 = STR 分派
 * + hash 惰性 + ≤4 步开放寻址（镜像 array.c 快路② + 刀6 的 tombstone/
 * absent 判据），步尽/非 STR 键/absent 全落 dimic_miss helper（norm/IC
 * fill/raise 语义逐条不变 = 结构消除非降级）。
 * fwd（值汇流）：逐层 MERGE+PHI（hit=LOAD pay 免 take 16B store、
 * base 步 miss=helper 成功写 dst 后 load）——与刀16 packed 单菱形同构，
 * 只是命中点从 1 变 ≤4 → 值沿层上传。fwd 关时恒 take thin（元素恒 INT
 * 由 guard_ht 证、槽遗留由 preheader 消灭证 = g->thin 双证）。 */

/* 递归直探步：p=0 用 home slot，1..3 为 (home+p)&hmask；步尽落 helper。
 * 返回 fwd phi 输入值（fwd 关 = 0，调用方不挂 phi）；返回后当前块 =
 * 本层 MERGE 合流块（父层自行 ir_END 收支）。 */
static ir_ref ht_probe_rec(DimIcCtx *g, ir_ref slot, ir_ref hm, ir_ref bk, int p) {
    JitC *j = g->j;
    JIT_CTX();
    if (p > IC_PROBE_STEPS) {
        dimic_miss(g, false);   /* IF_TRUE RETURN；FALSE 线留开 */
        return g->fwd ? load_pay(j, g->dst) : (ir_ref)0;
    }
    ir_ref st = p ? ir_AND(IR_U32, ir_ADD_U32(slot, ir_CONST_U32(p)), hm)
                  : slot;
    ir_ref zs = ir_ZEXT(IR_U64, st);
    ir_ref bp = ir_ADD_A(ir_ADD_A(bk, ir_SHL_U64(zs, ir_CONST_U64(4))),
                         ir_SHL_U64(zs, ir_CONST_U64(3)));   /* *24 */
    ir_ref bkey = ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, key)));
    /* 匹配 = 活桶 + key tag/pay 全身份（interned 串指针等 = C 侧
     * KEY_PTR_EQ||key_eq 的保守子集，miss 一律有 helper 兜底）；STR 分派
     * 已在外层，tag 恒比 g->kt（=T_STR）防混型桶误命中 */
    ir_ref m = ir_AND(IR_BOOL,
                ir_AND(IR_BOOL,
                       ir_EQ(g->kt, ir_LOAD_I32(
                           ir_ADD_A(bkey, ir_CONST_ADDR(TVAL_TAG)))),
                       ir_EQ(g->kp, ir_LOAD_I64(bkey))),
                ir_NE(ir_LOAD_U32(ir_ADD_A(bp,
                           ir_CONST_ADDR(offsetof(ABucket, val_off)))),
                      ir_CONST_U32(0xFFFFFFFFu)));
    ir_ref vh = 0, vsub = 0;
    ir_ref ifh = ir_IF(m);
    ir_IF_TRUE(ifh);
        ir_ref voff = ir_LOAD_U32(
            ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, val_off))));
        ir_ref data = ir_LOAD_A(ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, data))));
        ir_ref leaf = ir_ADD_A(data,
                     ir_SHL_U64(ir_ZEXT(IR_U64, voff), ir_CONST_U64(4)));
        if (g->fwd) vh = ir_LOAD_I64(leaf);
        else dimic_take(g, leaf, g->thin);
        ir_ref epT = ir_END();
    ir_IF_FALSE(ifh);
        vsub = ht_probe_rec(g, slot, hm, bk, p + 1);
        ir_ref epF = ir_END();
    ir_MERGE_2(epT, epF);
    if (g->fwd) return ir_PHI_2(IR_I64, vh, vsub);
    return 0;
}

/* spec_ht STR 线主体（emit_ht_fetch 抽出）：hash 惰性（lvl4 同款，h==0 →
 * tstr_hash 冷填写回合流 reload）+ ≤4 步开放寻址直探。ksa 来源分流：
 * kfwd = phi pay 位转 TStr*（槽无值）；否则 load。返回直探 fwd 输入
 * （v_str；fwd 关 = 0）。返回后当前块 = h0 合流 → probe 递归合流块。 */
static ir_ref ht_str_probe(DimIcCtx *g) {
    JitC *j = g->j;
    JIT_CTX();
    (void)j;
    ir_ref ksa = g->kfwd_mat
        ? ir_BITCAST_A(g->kfwd_pay) : ir_LOAD_A(g->key);
    ir_ref h0 = ir_LOAD_U32(ir_ADD_A(ksa, ir_CONST_ADDR(offsetof(TStr, hash))));
    ir_ref ifh0 = ir_IF(ir_EQ(h0, ir_CONST_U32(0)));
    ir_IF_TRUE(ifh0);
        /* 4b-i 顺手修：实参应为 TStr*（ksa），原传 g->key（TVal* 槽
         * 地址）→ tstr_hash 读错结构（len/hash 偏移全错、写回错位），
         * 与 lvl4 同款（1498 传 ksa）对齐；kfwd 下槽无值更不可传。 */
        ir_ref th[1] = { ksa };
        ir_CALL_N(IR_U32, ir_CONST_ADDR((uintptr_t)tstr_hash), 1, th);
        ir_ref ehT = ir_END();
    ir_IF_FALSE(ifh0);
        ir_ref ehF = ir_END();
    ir_MERGE_2(ehT, ehF);
    ir_ref h = ir_LOAD_U32(ir_ADD_A(ksa, ir_CONST_ADDR(offsetof(TStr, hash))));
    ir_ref hm = ir_LOAD_U32(ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, hmask))));
    ir_ref slot = ir_AND(IR_U32, h, hm);
    ir_ref bk = ir_LOAD_A(ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, buckets))));
    return ht_probe_rec(g, slot, hm, bk, 0);
}

/* spec_ht 点的 FETCH 发射（完整替代 inv/pcond/bound/链结构）。
 * fwd_on 由调用方（FETCH case）按刀16 同款形状门算好传入——is_jump_target
 * 等在 case 作用域。返回后当前块 = STR/非STR 合流块，fwd phi 已挂
 * j->spec_fwd_pay（fwd_on=1 时）。
 * 批4c（kt_fix）：键恒 T_STR 已证（1key guard / STR lit 编译期）→ 整个
 * STR 分派菱形免发——STR 主体直入当前块、non-STR 线不可达不发、fwd 直
 * 赋免外层 phi；kt 已由调用方预置 CONST T_STR（免槽 tag 装载）。 */
static void emit_ht_fetch(JitC *j, DimIcCtx *g, int pi, int lvl, int fwd_on) {
    JIT_CTX();
    g->fwd = fwd_on;
    /* 批4b-i：kfwd 时键 (tag,pay) 由 F1 phi 直供——槽恒不写，不得 load 槽；
     * 非 kfwd 照旧 load。kt 已在消费头预置（phi tag / 批4c CONST T_STR），
     * 此处仅补非 kfwd 非 kt_fix。 */
    if (!g->kfwd_mat && !g->kt_fix)
        g->kt = load_tag(j, g->key);
    g->a = j->pre_a[lvl][pi];
    ir_ref v_str = 0, v_no = 0;
    if (g->kt_fix) {
        v_str = ht_str_probe(g);           /* 无菱形直入（STR 证下 non-STR 线不可达） */
        if (fwd_on) j->spec_fwd_pay = v_str;
        return;
    }
    ir_ref ifstr = ir_IF(ir_EQ(g->kt, ir_CONST_I32(T_STR)));
    ir_IF_TRUE(ifstr);
        v_str = ht_str_probe(g);
        ir_ref eStrT = ir_END();
    ir_IF_FALSE(ifstr);
        dimic_miss(g, false);   /* 非 STR 键 → C 侧 norm/全语义兜底（先物化） */
        if (fwd_on) v_no = load_pay(j, g->dst);
        ir_ref eStrF = ir_END();
    ir_MERGE_2(eStrT, eStrF);
    if (fwd_on) j->spec_fwd_pay = ir_PHI_2(IR_I64, v_str, v_no);
}

/* 刀2：dst 与任一源操作数同槽判定——kind+index 双等才算（CV/TMP 下标域
 * 经 +n_locals 互不相交；CONST 的 lit 下标可能与槽下标撞数，kind 必查）。
 * 用于 both_int/标量叶等"分支已证源为标量"处跳过 rel_dst_gate。 */
static bool dst_src_alias(TOp *op) {
    return (op->t1 == op->tr && op->a1 == op->ar) ||
           (op->t2 == op->tr && op->a2 == op->ar);
}

static int mod_fast_enabled(void);      /* 定义在开关区（logstat 百菜），lower_binop 用 */
static int neg_fast_enabled(void);      /* 定义在开关区（object 刀），lower_un 用 */

static void lower_binop(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = slot_addr(j, op->ar);
    /* 批4b-ii：链加尾携带值（仅 spec 链式快线记录；慢线/非链恒 0 ⇒
     * dispatch 不挂 pay，消费端退化槽 load，见 dispatch 尾注） */
    j->spec_ctab_sum = 0;
    /* 类型特化批1：已证恒 INT 的位免 tag load（acc/idx=guard 证、
     * spec_tmp=全元素 INT 证）。ta 兼供慢线——const tag 即真值，恒正确。 */
    int spec_ta = spec_bit_int(j, op->t1, op->a1);
    ir_ref ta = spec_ta ? ir_CONST_I32(JT_INT) : load_tag(j, aa);
    ir_ref pa, tb, pb;
    /* 批4b-ii：链式待发表消费（先于刀16——双通道由 scan 模式分叉互斥，
     * spec_chain 时 spec_fwd 恒 0）。命中 ⇒ tag 恒 INT（guard 证元素恒
     * INT + 链加溢出 RETURN 不落 FLOAT）+ pay 取 phi（pay=0 = 槽已写，
     * load 照旧）；消费即清（ctab_take 内含 spec_tmp_valid 对齐）。 */
    ir_ref ct1_pay = 0, ct2_pay = 0;
    int ct1_unw = 0, ct2_unw = 0, ct2 = 0;
    int ct1 = ctab_take(j, op->t1, op->a1, &ct1_pay, &ct1_unw);
    if (ct1) { spec_ta = 1; ta = ir_CONST_I32(JT_INT); }
    /* 刀16：值直达消费（kind+idx 双等，防 CONST lit 撞数；消费即清，
     * 与 spec_tmp_valid 同生命周期——emit 期已证 pc+1 即本 ADD）。 */
    int fwd1 = !ct1 && j->spec_fwd_valid && op->t1 == j->spec_fwd_kind &&
               op->a1 == j->spec_fwd_idx;
    ir_ref fwd_val = 0;
    if (ct1) pa = ct1_pay ? ct1_pay : load_pay(j, aa);
    else if (fwd1) { pa = j->spec_fwd_pay; fwd_val = pa; j->spec_fwd_valid = 0; }
    else pa = load_pay(j, aa);
    int spec_tb = 0;
    if (op->t2 == OPD_CONST) {
        tb = ir_CONST_I32((int32_t)j->fn->lits[op->a2].t);
        pb = ir_CONST_I64(j->fn->lits[op->a2].u.i);
    } else {
        ir_ref ba = slot_addr(j, op->a2);
        spec_tb = spec_bit_int(j, op->t2, op->a2);
        tb = spec_tb ? ir_CONST_I32(JT_INT) : load_tag(j, ba);
        ct2 = ctab_take(j, op->t2, op->a2, &ct2_pay, &ct2_unw);
        if (ct2) { spec_tb = 1; tb = ir_CONST_I32(JT_INT); }
        int fwd2 = !ct2 && j->spec_fwd_valid && op->t2 == j->spec_fwd_kind &&
                   op->a2 == j->spec_fwd_idx;
        if (ct2) pb = ct2_pay ? ct2_pay : load_pay(j, ba);
        else if (fwd2) { pb = j->spec_fwd_pay; fwd_val = pb; j->spec_fwd_valid = 0; }
        else pb = load_pay(j, ba);
        if (spec_tb) j->spec_tmp_valid = 0;   /* 消费即清（时效见 spec_bit_int） */
    }
    if (op->op == OP_ADD || op->op == OP_SUB) {
        if (spec_ta && spec_tb) {
            /* 特化快线：双 INT 已证 → 免 both 检查与慢线；溢出分支发
             * RETURN(0)（acc 落 FLOAT 即破坏恒 INT 假设 → 后续跳过的
             * 检查致错值——安全证明链 type-spec.md 批1 ④）。FALSE 线
             * = 活线（无 MERGE，镜像 OP_SEND_CAP）。 */
            if (!dst_src_alias(op) && dst_gate_needed(j, op))
                rel_dst_gate(j, da);
            ir_ref sum = op->op == OP_ADD ? ir_ADD_I64(pa, pb)
                                          : ir_SUB_I64(pa, pb);
            /* 批4b-ii：链加尾携带值（链式 only）——dispatch 据此挂下一加
             * 的 pay（快线跑过 ⟺ 恒 INT 成立；慢线/非链恒 0 不挂）。 */
            if (j->spec_chain) j->spec_ctab_sum = sum;
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
                /* 刀16：deopt 物化——OSR 从本 ADD pc 重跑读 tmp 槽，
                 * 值直达下槽恒 UNDEF → RETURN 前补写（冷路 2 store）。
                 * 槽恒 UNDEF（preheader 消灭 + 直达不写）→ 裸存安全。 */
                if (fwd_val)
                    store_imm(j, slot_addr(j, j->spec_fwd_idx),
                              (int32_t)JT_INT, fwd_val);
                /* 批4b-ii：链式消费的 phi 直达操作数 RETURN 前补写（槽未
                 * 写；rel_dst_gate 自带释放门对任意旧值安全，不依赖
                 * preheader 消灭）。活跃集 ⊆ 本加操作数（结构证：A1 消费
                 * {F1,F2} 全活跃、A_i 消费 {前链,本 fetch} 全活跃）⇒ 只补
                 * ct1/ct2；unwritten=0（槽已 store/链 dst 恒写）恒跳。 */
                if (ct1 && ct1_unw) {
                    rel_dst_gate(j, aa);
                    store_imm(j, aa, JT_INT, ct1_pay);
                }
                if (ct2 && ct2_unw) {
                    ir_ref ba2 = slot_addr(j, op->a2);
                    rel_dst_gate(j, ba2);
                    store_imm(j, ba2, JT_INT, ct2_pay);
                }
                ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
            ir_IF_FALSE(ifof);
                store_imm(j, da, JT_INT, sum);
        } else {
        ir_ref both = ir_AND(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_INT)),
                             ir_EQ(tb, ir_CONST_I32(JT_INT)));
        ir_ref ifx = ir_IF(both);
        ir_IF_TRUE(ifx);
            /* 刀2 释放门：快线裸存前 release 旧 dst（wtake 语义）。
             * both_int 分支内 dst 若即源槽（$x=$x+1 形态）必为 INT →
             * 标量免释，emit 判别跳过（慢路 binop_slow 自释放不发）；
             * 刀3：dst 槽全写者标量 → 槽级免发 */
            if (!dst_src_alias(op) && dst_gate_needed(j, op))
                rel_dst_gate(j, da);
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
                /* 溢出 → 数值转 double 相加（镜像 tval_add to_num；旧版
                 * BITCAST_D 位重解释得 NaN——批1 溢出对拍矩阵首暴露） */
                ir_ref fd = ir_ADD_D(ir_INT2D(pa), ir_INT2D(pb));
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
        }
    } else if (op->op == OP_MOD && mod_fast_enabled()) {
        /* logstat 百菜第一点（11-10）：MOD int 快线——双 INT 已证时 IR
         * IR_MOD 直出（x86 后端 idiv），免 CALL tval_mod 的类型分发+to_int
         * 框架；除数 0 显式落既有 slow（DivisionByZeroError 逐位原样，
         * 亦避开 idiv #DE）；除数 -1 恒 0（PHP 语义，与 tval_mod 慢线修复
         * 同式，消 INT64_MIN/-1 UB）；非双 INT → slow 原样（float 取整
         * mod 语义在 tval_mod 内）。开关 TPHP_MOD_FAST=0 = HEAD 恒 slow。 */
        ir_ref bothm = ir_AND(IR_BOOL,
            ir_EQ(ta, ir_CONST_I32(JT_INT)),
            ir_EQ(tb, ir_CONST_I32(JT_INT)));
        ir_ref ifm = ir_IF(bothm);
        ir_IF_TRUE(ifm);
            ir_ref ifz = ir_IF(ir_EQ(pb, ir_CONST_I64(0)));
            ir_IF_TRUE(ifz);
                call_slow_binop(j, op->op, da, ta, pa, tb, pb);   /* raise */
                ir_ref eZ = ir_END();
            ir_IF_FALSE(ifz);
                if (!dst_src_alias(op) && dst_gate_needed(j, op))
                    rel_dst_gate(j, da);   /* wtake 语义；slow 分支自释放不发 */
                ir_ref ifm1 = ir_IF(ir_EQ(pb, ir_CONST_I64(-1)));
                ir_IF_TRUE(ifm1);
                    store_imm(j, da, JT_INT, ir_CONST_I64(0));
                    ir_ref eM1 = ir_END();
                ir_IF_FALSE(ifm1);
                    store_imm(j, da, JT_INT, ir_MOD_I64(pa, pb));
                    ir_ref eM2 = ir_END();
                ir_MERGE_2(eM1, eM2);
                ir_ref eNZ = ir_END();
            ir_MERGE_2(eZ, eNZ);
            ir_ref eT = ir_END();
        ir_IF_FALSE(ifm);
            call_slow_binop(j, op->op, da, ta, pa, tb, pb);
            ir_ref eS = ir_END();
        ir_MERGE_2(eT, eS);
    } else {
        call_slow_binop(j, op->op, da, ta, pa, tb, pb);
    }
}

static void lower_cmp(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = slot_addr(j, op->ar);
    /* 类型特化批4a §8.3-5：cond 区恒 INT 位免 tag 装载（acc/idx=guard 证、
     * spec_tmp=元素证——proof 下 ta/tb 恒真值，慢线分发不受影响） */
    ir_ref ta = spec_bit_int(j, op->t1, op->a1)
              ? ir_CONST_I32(JT_INT) : load_tag(j, aa);
    ir_ref pa = load_pay(j, aa);
    ir_ref tb, pb;
    if (op->t2 == OPD_CONST) {
        tb = ir_CONST_I32((int32_t)j->fn->lits[op->a2].t);
        pb = ir_CONST_I64(j->fn->lits[op->a2].u.i);
    } else {
        ir_ref ba = slot_addr(j, op->a2);
        tb = spec_bit_int(j, op->t2, op->a2)
           ? ir_CONST_I32(JT_INT) : load_tag(j, ba);
        pb = load_pay(j, ba);
    }
    ir_ref both_int = ir_AND(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_INT)),
                             ir_EQ(tb, ir_CONST_I32(JT_INT)));
    if (op->op == OP_CMP) {
        ir_ref ifx = ir_IF(both_int);
        ir_IF_TRUE(ifx);
            if (!dst_src_alias(op) && dst_gate_needed(j, op))
                rel_dst_gate(j, da);   /* 刀2 释放门 + 刀3 槽级免发 */
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
        if (!dst_src_alias(op) && dst_gate_needed(j, op))
            rel_dst_gate(j, da);   /* 刀2 释放门 + 刀3 槽级免发 */
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

/* 无分支 bool 存储：tag = T_FALSE + SEXT(cond)（FALSE=1, TRUE=2）。
 * 钻石 MERGE 嵌在 LOOP 区域会触发 IR codegen 崩溃（ir_check 都拦不住），
 * 值选择一律走位运算；控制流 IF 只留给 JMPZ。 */
/* 无分支 mask：cond BOOL → i64 0/-1；i32 版 0/1 */
/* 批2-6：tv_copy 内联（镜像 tphp_jit_tv_copy：v=*src; addref(v);
 * release(dst); *dst=v——顺序严格同 C，先 addref 后 release 防同对象
 * 自指归零；块结构与 dimic_take 的 addref/release 段同款）。仅 OSR chunk
 * 走此路径：helper 调用的两个 slot-addr 实参跨前置 call 活跃，会踩
 * deps/ir LSRA 实参 hint 窗口 bug（详见 lower_un OP_COPY 注释）。 */
static void lower_tv_copy_inline(JitC *j, ir_ref da, ir_ref sa) {
    JIT_CTX();
    ir_ref vtag = ir_LOAD_I32(ir_ADD_A(sa, ir_CONST_ADDR(TVAL_TAG)));

    /* --- addref v --- */
    ir_ref ifv = ir_IF(ir_AND(IR_BOOL,
        ir_GE(vtag, ir_CONST_I32(T_STR)), ir_LE(vtag, ir_CONST_I32(T_CLOSURE))));
    ir_IF_TRUE(ifv);
        ir_ref gcv = ir_LOAD_A(sa);
        ir_ref fv = ir_ZEXT(IR_I32,
            ir_LOAD_U8(ir_ADD_A(gcv, ir_CONST_ADDR(offsetof(GCHead, flags)))));
        ir_ref ifiv = ir_IF(ir_EQ(ir_AND_I32(fv, ir_CONST_I32(GC_FLAG_INTERNED)),
                                  ir_CONST_I32(0)));
        ir_IF_TRUE(ifiv);
            ir_ref rcv = ir_LOAD_I32(gcv);
            ir_STORE(gcv, ir_ADD_I32(rcv, ir_CONST_I32(1)));
            ir_ref eV1 = ir_END();
        ir_IF_FALSE(ifiv);
            ir_ref eV0 = ir_END();
        ir_MERGE_2(eV1, eV0);
        ir_ref eRefV = ir_END();
    ir_IF_FALSE(ifv);
        ir_ref eScV = ir_END();
    ir_MERGE_2(eRefV, eScV);

    /* --- release dst --- */
    ir_ref td = ir_LOAD_I32(ir_ADD_A(da, ir_CONST_ADDR(TVAL_TAG)));
    ir_ref ifd = ir_IF(ir_AND(IR_BOOL,
        ir_GE(td, ir_CONST_I32(T_STR)), ir_LE(td, ir_CONST_I32(T_CLOSURE))));
    ir_IF_TRUE(ifd);
        ir_ref gcd = ir_LOAD_A(da);
        ir_ref fd = ir_ZEXT(IR_I32,
            ir_LOAD_U8(ir_ADD_A(gcd, ir_CONST_ADDR(offsetof(GCHead, flags)))));
        ir_ref ifid = ir_IF(ir_EQ(ir_AND_I32(fd, ir_CONST_I32(GC_FLAG_INTERNED)),
                                  ir_CONST_I32(0)));
        ir_IF_TRUE(ifid);
            ir_ref rcd = ir_LOAD_I32(gcd);
            ir_ref ifrc = ir_IF(ir_GT(rcd, ir_CONST_I32(1)));
            ir_IF_TRUE(ifrc);
                ir_STORE(gcd, ir_SUB_I32(rcd, ir_CONST_I32(1)));
                ir_ref eDec = ir_END();
            ir_IF_FALSE(ifrc);
                ir_ref tr[1] = { da };
                ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, tr);
                ir_ref eSlow = ir_END();
            ir_MERGE_2(eDec, eSlow);
            ir_ref eD1 = ir_END();
        ir_IF_FALSE(ifid);
            ir_ref eD0 = ir_END();
        ir_MERGE_2(eD1, eD0);
        ir_ref eRefD = ir_END();
    ir_IF_FALSE(ifd);
        ir_ref eScD = ir_END();
    ir_MERGE_2(eRefD, eScD);

    /* --- 16B store（pay 先、tag 后；sa==da 自拷贝时 addref 已保活） --- */
    ir_STORE(da, ir_LOAD_I64(sa));
    ir_STORE(ir_ADD_A(da, ir_CONST_ADDR(TVAL_TAG)), vtag);
}

static void lower_un(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = slot_addr(j, op->ar);
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    switch (op->op) {
    case OP_COPY: case OP_ASSIGN:
        /* P0-1：裸 store 对数组/串双重释放 → 走 rc helper（标量 no-op）。
         * 批2-6（2026-11-02，linux CI test-osr-005 c2 段错误根因）：OSR
         * chunk 内改内联——tv_copy 的两个 slot-addr 实参跨前置 opseq call
         * 活跃，deps/ir LSRA 按 SysV 实参 hint 把 src 钉进 call 易失的 RSI
         * 且不回装（ARGDBG 实锤 src==dst==RSI 而值在栈 spill；emit 按
         * "已在寄存器"跳过装载 → 垃圾实参 → TypeError）。Win64 因 hint
         * RDX 竞争败给 preserved r14 而侥幸未现。上游 ir_ra.c 与 HEAD
         * 一致（无现成修复）；内联镜像 tphp_jit_tv_copy 语义（addref src
         * → release dst → 16B store），chunk 内不再有该调用形态。 */
        /* 刀2 连带修复（11-04 foreach×cmp 门崩溃）：非 OSR 形态一并内联——
         * 实测函数级 emit 下，lower_cmp 新增释放门（IF+CALL+MERGE）扰动
         * LSRA 后，本 CALL 两实参被 hint 进易失实参寄存器且不回装，
         * tphp_jit_tv_copy(src=0x1) 0xC0000005（m_b/m_g/m_i 最小复现，
         * 关门即消失）。与批2-6 OSR 内联同族根因（deps/ir LSRA 实参 hint
         * 窗口 bug，上游 ir_ra.c 无现成修复）→ 调用形态整体撤除。 */
        lower_tv_copy_inline(j, da, aa);
        break;
    case OP_BOOL_NOT: {
        ir_ref t = make_truthy(j, ta, pa, aa);
        /* 刀2 释放门：结果恒标量而 dst 无约束（dst 即源槽时源可能是串，
         * 不释放即漏）→ 恒发，不可 emit 跳过；刀3 槽级免发除外 */
        if (dst_gate_needed(j, op)) rel_dst_gate(j, da);
        store_bool(j, da, ir_NOT_B(t));
        break;
    }
    case OP_BOOL_VAL: {
        ir_ref t = make_truthy(j, ta, pa, aa);
        if (dst_gate_needed(j, op))
            rel_dst_gate(j, da);   /* 刀2 释放门（同 BOOL_NOT：恒发；刀3 除
                                    * 外） */
        store_bool(j, da, t);
        break;
    }
    case OP_NEG: {
        /* object 刀（11-13）：INT 快线——tag==INT 且 pay!=INT64_MIN 时
         * IR 直接取负（免 CALL tval_neg 的 to_num 分发框架）；INT64_MIN
         * 落 slow（→float，PHP 语义）、非 INT（float/str/bool...）落 slow
         * 原样。释放门镜像 MOD 快线（slow 分支自释放不发；dst==src 别名
         * 时源已是本值不发）。开关 TPHP_NEG_FAST=0 = HEAD 恒 slow。 */
        if (!neg_fast_enabled()) {
            call_slow_binop(j, OP_NEG, da, ta, pa, ta, pa);
            break;
        }
        ir_ref ifni = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_INT)));
        ir_IF_TRUE(ifni);
            ir_ref ifmin = ir_IF(ir_EQ(pa, ir_CONST_I64(INT64_MIN)));
            ir_IF_TRUE(ifmin);
                call_slow_binop(j, OP_NEG, da, ta, pa, ta, pa);   /* →float */
                ir_ref eMin = ir_END();
            ir_IF_FALSE(ifmin);
                if (!dst_src_alias(op) && dst_gate_needed(j, op))
                    rel_dst_gate(j, da);
                store_imm(j, da, JT_INT, ir_SUB_I64(ir_CONST_I64(0), pa));
                ir_ref eNeg = ir_END();
            ir_MERGE_2(eMin, eNeg);
            ir_ref eInt = ir_END();
        ir_IF_FALSE(ifni);
            call_slow_binop(j, OP_NEG, da, ta, pa, ta, pa);
            ir_ref eSlowN = ir_END();
        ir_MERGE_2(eInt, eSlowN);
        break;
    }
    case OP_POS:
        call_slow_binop(j, OP_POS, da, ta, pa, ta, pa);
        break;
    case OP_CAST: {
        int64_t ct = j->fn->lits[op->a2].u.i;   /* 1=int 2=bool 3=float */
        if (ct == 2) {
            ir_ref t = make_truthy(j, ta, pa, aa);
            if (dst_gate_needed(j, op))
                rel_dst_gate(j, da);   /* 刀2 释放门（结果标量、dst 无约束→恒
                                        * 发；刀3 槽级免发除外） */
            store_bool(j, da, t);
        } else if (ct == 1) {
            /* 刀2 修复（错值+泄漏双 bug）：
             * - 旧链 FLOAT 叶 FP2I64 裸转——NAN/超范围 tval_to_int 应
             *   raise 却静默转垃圾；旧最终 else 对 STR/ARR 恒存 0——
             *   (int)"5" 应得 5（probe ci 实测 JIT on 0、off 5）。
             * - 新：快叶收窄为 INT/TRUE/NULL/FALSE（tval_to_int 不 raise
             *   且可内联的全部形态），FLOAT/STR/ARR/UNDEF 落慢路 helper
             *   （语义源 tval_to_int，raise 走 setjmp→deopt）。
             * - 快叶写前释放旧 dst：别名 $s=(int)$s 时分支条件已证源为
             *   标量 → 门空转，emit 判别跳过；慢路 helper 自释放，绝不在
             *   ELSE 前置门（四象限：别名/非别名 × 快/慢均单次释放）。 */
            ir_ref fast = ir_OR(IR_BOOL,
                ir_EQ(ta, ir_CONST_I32(JT_INT)),
                ir_OR(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_TRUE)),
                    ir_OR(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_NULL)),
                          ir_EQ(ta, ir_CONST_I32(JT_FALSE)))));
            ir_ref iff = ir_IF(fast);
            ir_IF_TRUE(iff);
                if (!(op->t1 == op->tr && op->a1 == op->ar) &&
                    dst_gate_needed(j, op))
                    rel_dst_gate(j, da);
                ir_ref ifi = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_INT)));
                ir_IF_TRUE(ifi);
                    store_imm(j, da, JT_INT, pa);
                    ir_ref eI = ir_END();
                ir_IF_FALSE(ifi);
                    ir_ref ift = ir_IF(ir_EQ(ta, ir_CONST_I32(JT_TRUE)));
                    ir_IF_TRUE(ift);
                        store_imm(j, da, JT_INT, ir_CONST_I64(1));
                        ir_ref eT = ir_END();
                    ir_IF_FALSE(ift);
                        store_imm(j, da, JT_INT, ir_CONST_I64(0));
                        ir_ref e0 = ir_END();
                    ir_MERGE_2(eT, e0);
                    ir_ref eRest = ir_END();
                ir_MERGE_2(eI, eRest);
                ir_ref eFast = ir_END();
            ir_IF_FALSE(iff);
                call_slow_cast(j, 1, da, ta, pa);
                ir_ref eSlow = ir_END();
            ir_MERGE_2(eFast, eSlow);
        } else {
            /* ct==3（float）：快叶 FLOAT/INT/TRUE/NULL/FALSE（tval_to_float
             * 对其全不 raise）；STR/ARR/OBJ 等落慢路——旧链对 STR 静默给
             * 0.0，(float)"5.5" 应得 5.5（probe cf 实测 off 得 5.5）。 */
            ir_ref fast = ir_OR(IR_BOOL,
                ir_EQ(ta, ir_CONST_I32(JT_FLOAT)),
                ir_OR(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_INT)),
                    ir_OR(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_TRUE)),
                        ir_OR(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_NULL)),
                              ir_EQ(ta, ir_CONST_I32(JT_FALSE))))));
            ir_ref iff = ir_IF(fast);
            ir_IF_TRUE(iff);
                if (!(op->t1 == op->tr && op->a1 == op->ar) &&
                    dst_gate_needed(j, op))
                    rel_dst_gate(j, da);
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
                ir_ref eFast = ir_END();
            ir_IF_FALSE(iff);
                call_slow_cast(j, 3, da, ta, pa);
                ir_ref eSlow = ir_END();
            ir_MERGE_2(eFast, eSlow);
        }
        break;
    }
    default: break;
    }
}

/* INC/DEC suffix 的 dst 写（do_incdec res=old 镜像）：UNDEF→NULL 归一 +
 * 非标量 addref——tv_copy 不做 UNDEF→NULL，故独立小件。仅慢形态被调
 * （标量走 IR 直存，热路径 $i++ 零调用——批2-2 实测 tv_copy 每迭代
 *  一次使 array-read +33%、kv +9%，标量直存后归零）。 */
static void tphp_jit_inc_olddst(TVal *dst, TVal *src) {
    TVal old = src->t == T_UNDEF ? TV_NULL : *src;
    tval_addref(old);
    tval_release(dst);
    *dst = old;
}

/* 死 dst 检测（保守）：dst 槽在全函数（除本 op）零提及 → 不可能被读 →
 * 跳过写。for 的 $i++ 经 tmp_dst 恒建 o3 tmp 但从不读，每迭代白付
 * tag/payload 双载+分支+双存（array-read +14%、kv +1.3ms 实测）。
 * 正确性只依赖"零提及=不可读"：任何提及（含他 op 写复用同槽）都保留
 * 写 → 仅省略确定死的写。 */
static bool dst_slot_unused(TFunc *fn, uint32_t skip_pc, uint8_t kind, uint32_t slot) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        if (i == skip_pc) continue;
        TOp *o = &fn->ops[i];
        if ((o->t1 == kind && o->a1 == slot) ||
            (o->t2 == kind && o->a2 == slot) ||
            (o->tr == kind && o->ar == slot))
            return false;
    }
    return true;
}

static void lower_incdec(JitC *j, TOp *op, uint32_t pc) {
    JIT_CTX();
    if (op->t2 != OPD_UNUSED) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return; } while (0); }  /* dim 形态 $a[$i]++ 走解释器（本 lower 只镜像 var-RMW） */
    ir_ref aa = slot_addr(j, op->a1), da = aa;
    bool has_dst = (op->tr != OPD_UNUSED) &&
                   !dst_slot_unused(j->fn, pc, op->tr, op->ar);
    bool prefix = op->ext != 0;
    ir_ref dstA = has_dst ? slot_addr(j, op->ar) : aa;
    /* 类型特化批1：$i 恒 INT 已证（guard + 溢出 RETURN）→ 免 tag 检查
     * 与慢线。ta 供 suffix/prefix/res 动态判定原样 load（spec 时若无
     * 消费者由 IR DCE 消死码）。 */
    int spec_i = spec_bit_int(j, op->t1, op->a1);
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    if (has_dst && !prefix) {
        /* suffix（res=old）：src 未更新，标量旧值直存；非标量/UNDEF →
         * helper（UNDEF→NULL + addref，与 do_incdec 逐位同语义） */
        ir_ref ifs = ir_IF(ir_LT(ta, ir_CONST_I32(5)));   /* T_STR=5 起非标量 */
        ir_IF_TRUE(ifs);
            /* 刀2 释放门：dst 即源槽（$x++ 形态）时分支已证其为标量 →
             * 免发；dst≠源槽时 dst 旧值可能是串，裸存前必释放；刀3 槽级
             * 免发除外（dst 槽全写者标量） */
            if (!(op->tr == op->t1 && op->ar == op->a1) &&
                dst_gate_needed(j, op))
                rel_dst_gate(j, dstA);
            store_pay(j, dstA, pa);          /* 动态 tag/payload：store_imm 的 */
            store_tag(j, dstA, ta);          /* tag 是立即数，传 ref 会存 ref-id */
            ir_ref eS = ir_END();
        ir_IF_FALSE(ifs);
            ir_ref th[2] = { dstA, aa };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_inc_olddst), 2, th);
            ir_ref eH = ir_END();
        ir_MERGE_2(eS, eH);
    }
    ir_ref d1 = op->op == OP_INC ? ir_CONST_I64(1) : ir_CONST_I64(-1);
    if (spec_i) {
        /* 特化快线（类型特化批1）：$i 恒 INT → 免 isint 检查与慢线；
         * 溢出分支发 RETURN(0)（绝不落 FLOAT，保假设——安全证明链
         * type-spec.md 批1 ④）。FALSE 线 = 活线（无 MERGE，镜像
         * OP_SEND_CAP）。 */
        ir_ref sum = ir_ADD_I64(pa, d1);
        ir_ref of = ir_LT(ir_AND_I64(ir_XOR_I64(pa, sum),
                                     ir_XOR_I64(pa, d1)),
                          ir_CONST_I64(0));
        ir_ref ifof = ir_IF(of);
        ir_IF_TRUE(ifof);
            /* 批4b-ii：INC 可插在 F/链加之间（scan 允许）——此时活跃
             * = {前链, 本 fetch} ⇒ 全表物化后 RETURN（镜像 dimic_miss 出口）。 */
            if (j->spec_chain) ctab_mat_all(j);
            ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
        ir_IF_FALSE(ifof);
            store_imm(j, da, JT_INT, sum);
    } else {
    ir_ref isint = ir_EQ(ta, ir_CONST_I32(JT_INT));
    ir_ref ifx = ir_IF(isint);
    ir_IF_TRUE(ifx);
        ir_ref sum = ir_ADD_I64(pa, d1);
        ir_ref of = ir_LT(ir_AND_I64(ir_XOR_I64(pa, sum),
                                     ir_XOR_I64(pa, d1)),
                          ir_CONST_I64(0));
        ir_ref ifof = ir_IF(of);
        ir_IF_TRUE(ifof);
            double fd = op->op == OP_INC ? 1.0 : -1.0;
            /* 溢出 → 数值转 double（旧版 BITCAST_D 位重解释得 NaN，
             * 与 tval inc 慢路发散——批1 溢出对拍矩阵首暴露） */
            ir_ref fdv = ir_ADD_D(ir_INT2D(pa), ir_CONST_DOUBLE(fd));
            store_imm(j, da, JT_FLOAT, ir_BITCAST_U64(fdv));
            ir_ref eOvf = ir_END();
        ir_IF_FALSE(ifof);
            store_imm(j, da, JT_INT, sum);
            ir_ref eInt = ir_END();
        ir_MERGE_2(eOvf, eInt);
        ir_ref eFast = ir_END();
    ir_IF_FALSE(ifx);
        double fd2 = op->op == OP_INC ? 1.0 : -1.0;
        /* 走 call_slow_binop 封装（P0-12：内含 deopt 分支） */
        call_slow_binop(j, op->op == OP_INC ? OP_ADD : OP_SUB, da, ta, pa,
                        ir_CONST_I32(JT_FLOAT),
                        ir_BITCAST_U64(ir_CONST_DOUBLE(fd2)));
        ir_ref eSlow = ir_END();
    ir_MERGE_2(eFast, eSlow);
    }
    if (has_dst && prefix) {
        /* prefix（res=nv）：src 已是新值——标量直存；非标量（如 "abc"++
         * 增量得新串）→ tv_copy（release+copy+addref） */
        ir_ref nt = load_tag(j, aa), np = load_pay(j, aa);
        ir_ref ifp = ir_IF(ir_LT(nt, ir_CONST_I32(5)));
        ir_IF_TRUE(ifp);
            /* 刀2 释放门（同 suffix 标量叶）：dst==源槽免发，否则先释放；
             * 刀3 槽级免发除外 */
            if (!(op->tr == op->t1 && op->ar == op->a1) &&
                dst_gate_needed(j, op))
                rel_dst_gate(j, dstA);
            store_pay(j, dstA, np);
            store_tag(j, dstA, nt);
            ir_ref eP = ir_END();
        ir_IF_FALSE(ifp);
            ir_ref tp[2] = { dstA, aa };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_tv_copy), 2, tp);
            ir_ref ePC = ir_END();
        ir_MERGE_2(eP, ePC);
    }
}

/* copy folded-call arg k into dst TVal (packed: kind<<24 | idx) — SEND
 * operands may be CONST literals, not slots (2026-10-28: strlen(1,2,3)
 * sent consts, loaded lits idx as slot idx -> garbage TVal into builtin) */
static void arg_copy(JitC *j, ir_ref dst, uint32_t k) {
    JIT_CTX();
    uint32_t packed = j->p_arg_slot[k];
    uint8_t kind = (uint8_t)(packed >> 24);
    uint32_t idx = packed & 0xFFFFFF;
    if (kind == OPD_CONST) {
        TVal v = j->fn->lits[idx];
        store_imm(j, dst, (int32_t)v.t, ir_CONST_I64(v.u.i));
    } else {
        ir_ref sa = slot_addr(j, idx);
        store_pay(j, dst, load_pay(j, sa));
        store_tag(j, dst, load_tag(j, sa));
    }
}

/* P1-12 批2-4：callee 体是否无帧——直调链不推 VMFrame（slots 显式传参），
 * 走 lower_replay 的真 handler 读 vm->x.frame->slots 会落进调用方帧 →
 * 只允许无帧体走折叠直调，含 replay/pad op 的体回退 p_seq 序回放
 * （op_do_fcall 推帧后经 opseq 执行，原路径）。 */
static int prop_helper_enabled(void);   /* 定义在开关区（刀5，晚于本用点） */
static int call_str_slow_enabled(void); /* 定义在开关区（刀19，晚于本用点） */
static bool jit_body_frameless(TFunc *fn) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        switch (fn->ops[i].op) {
        /* 刀5：FETCH_PROP 摘除（helper 开关开时）——已原生发射为
         * tphp_jit_fetch_prop helper（vm.c 造本地假帧后跑真 handler，
         * slots 指调用链传入槽区），不依赖 ambient 帧 → getter 体可走
         * 折叠直调（mcall fold 解锁）。开关关 → 仍拒（回放需帧）。 */
        case OP_FETCH_PROP:
            if (prop_helper_enabled()) break;
            return false;
        case OP_ASSIGN_PROP:
        case OP_ISSET_PROP: case OP_UNSET_PROP:
        case OP_ISSET_DIM:
        case OP_FE_RESET: case OP_FE_FETCH:
        case OP_INIT_NEW: case OP_NEW_USER:
        case OP_INIT_MCALL:
            return false;
        default: break;
        }
    }
    return true;
}

/* 批2-5：无 ctor new 折叠直落（镜像 new_user_at 无构造器路径，净额等价：
 * alloc rc=1 → dst 接管（解释器 rc++/-- 后 transfer 同净额）；无 dst =
 * 直接 release 归零（解释器 rc-- + release 同净额）。alloc 先于 dst
 * release = 镜像解释器顺序（GC 扫描时旧 dst 仍在槽内可达）。无 raise 面
 * 由 INIT_NEW 的 emit 检查保证（ic 固化 + m_ctor<0 + 非 abstract/非
 * Closure + 属性无 def_thunk）→ 零守卫零 setjmp。 */
static void lower_new_finish(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref ta[1] = { ir_CONST_ADDR((uintptr_t)j->p_new_cls) };
    ir_ref o = ir_CALL_N(IR_I64,
                         ir_CONST_ADDR((uintptr_t)tphp_obj_alloc), 1, ta);
    if (op->tr != OPD_UNUSED) {
        ir_ref da = slot_addr(j, op->ar);
        ir_ref tr[1] = { da };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, tr);
        ir_STORE(da, o);
        ir_STORE(ir_ADD_A(da, ir_CONST_ADDR(TVAL_TAG)), ir_CONST_I32(T_OBJ));
    } else {
        ir_ref sc = j->r_call_ret;
        ir_STORE(sc, o);
        ir_STORE(ir_ADD_A(sc, ir_CONST_ADDR(TVAL_TAG)), ir_CONST_I32(T_OBJ));
        ir_ref tr[1] = { sc };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, tr);
    }
    j->p_new = 0;
    j->p_new_cls = NULL;
    j->p_seq = 0;   /* 序态一并清（与 lower_replay 同位） */
}

/* 批2-8：user-ctor 折叠直调（规格 memory/2026-11-03 批2-8 节）。
 * 净额逐位镜像 new_user_at + ctor-RETURN + unwind half_obj：
 *   成功 = alloc(rc1) + [slot0 raw+addref → direct call → release]（净 0）
 *          + release(ret_sc)（丢弃 ctor 返回值——tval_release(&r) 同款；
 *          call 前先把 ret_sc 置 UNDEF，void 返回不读陈旧值）
 *          + dst release+raw store（不 addref → rc1 归 dst——ctor-RETURN
 *          "move 禁 addref"（P0-2）的对偶）
 *   raise = release(slot0 addref) → half_cleanup（DTORED 跳过 dtor +
 *          收掉 base——vm_unwind_core:4483-4488 同款）→ RETURN（异常链
 *          直返，无帧可弹故不走 unwind）
 * 守卫（entry belt + 参数 INT 契约）全在 alloc 之前 → 失配零垃圾：
 * 函数级收 mfail（回放重跑调用序；表达式已求值、opseq 跳过恰正确），
 * OSR RETURN p_seq_pc（INIT_NEW 幂等重跑，与 mcall OSR 同权衡）。 */
static void tphp_jit_ctor_half_cleanup(TObj *o) {
    o->h.flags |= GC_FLAG_DTORED;   /* 构造失败不跑 __destruct（PHP 语义） */
    TVal ov = TV_OBJ(o);
    tval_release(&ov);
}

/* 有值 return 的 ctor 不折：callee 的 generic move 会把 $this 移出 slot0
 * （return $this → UNDEF），而 dst 落点读 slot0——解释器 is_ctor 路不 move
 * （vm.c:1813 丢弃 r 后读 slot0），两端会分叉。PHP ctor 极少返回值 →
 * 保守放弃折叠零实际损失。 */
static bool jit_ctor_ret_val(TFunc *fn) {
    for (uint32_t i = 0; i < fn->n_ops; i++)
        if (fn->ops[i].op == OP_RETURN && fn->ops[i].t1 != OPD_UNUSED)
            return true;
    return false;
}

static void lower_new_ctor_finish(JitC *j, TOp *op, TFunc *cfn, TClass *ncls) {
    JIT_CTX();
    uint32_t argc = j->p_argc;
    bool mf = !j->osr;
    uint32_t nslots = (uint32_t)cfn->n_locals + cfn->n_tmps;
    ir_ref slots_sc = (nslots <= 9) ? j->r_call_args
        : ir_ALLOCA(ir_CONST_I64((uintptr_t)nslots * TVAL_SZ));
    /* slot0=UNDEF（守卫后填）、尾部 locals/tmps UNDEF、参数 raw 拷入 1.. */
    ir_ref d0 = ir_ADD_A(slots_sc, ir_CONST_ADDR(0));
    store_imm(j, d0, (int32_t)T_UNDEF, ir_CONST_I64(0));
    for (uint32_t k = 1 + argc; k < nslots; k++) {
        ir_ref d = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
        store_imm(j, d, (int32_t)T_UNDEF, ir_CONST_I64(0));
    }
    for (uint32_t k = 0; k < argc; k++) {
        ir_ref d = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)(1 + k) * TVAL_SZ));
        arg_copy(j, d, k);
    }
    ir_ref ret_sc = j->r_call_ret;
    store_imm(j, ret_sc, (int32_t)T_UNDEF, ir_CONST_I64(0));   /* void 返回不读陈旧 */
    /* --- 守卫 1：entry belt（静态门已查，运行时 belt 同 mcall） --- */
    ir_ref entry = ir_LOAD_I64(ir_ADD_A(ir_CONST_ADDR((uintptr_t)cfn),
                                        ir_CONST_ADDR(offsetof(TFunc, jit_entry))));
    ir_ref ife = ir_IF(ir_EQ(entry, ir_CONST_I64(0)));
    ir_IF_TRUE(ife);
        if (mf) j->mfail[j->mfail_n++] = ir_END();
        else ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));
    ir_IF_FALSE(ife);
    /* --- 守卫 2：参数 INT 契约（P1-12 JIT→JIT 直调合同） --- */
    for (uint32_t k = 0; k < argc; k++) {
        ir_ref a = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)(1 + k) * TVAL_SZ));
        ir_ref tg = ir_LOAD_I32(ir_ADD_A(a, ir_CONST_ADDR(TVAL_TAG)));
        ir_ref ift = ir_IF(ir_NE(tg, ir_CONST_I32(T_INT)));
        ir_IF_TRUE(ift);
            if (mf) j->mfail[j->mfail_n++] = ir_END();
            else ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));
        ir_IF_FALSE(ift);
    }
    /* --- 守卫全过 → alloc（失败路零垃圾：未分配，raw 实参弃之无害） --- */
    ir_ref ta[1] = { ir_CONST_ADDR((uintptr_t)ncls) };
    ir_ref o = ir_CALL_N(IR_I64, ir_CONST_ADDR((uintptr_t)tphp_obj_alloc), 1, ta);
    store_pay(j, d0, o);
    store_tag(j, d0, ir_CONST_I32(T_OBJ));
    ir_STORE(o, ir_ADD_I32(ir_LOAD_I32(o), ir_CONST_I32(1)));   /* ctor 调用引用 */
    ir_ref t[3] = { j->r_vm, slots_sc, ret_sc };
    ir_ref ok = ir_CALL_N(IR_I32, entry, 3, t);
    /* --- raise：镜像 unwind half_obj（4483-4488） --- */
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_ref optr = ir_LOAD_A(d0);
        ir_ref trd[1] = { d0 };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, trd);
        ir_ref hc[1] = { optr };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_ctor_half_cleanup), 1, hc);
        ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));
    ir_IF_FALSE(ifd);
    /* --- 成功：归还 $this addref（2→1）、丢弃 ctor 返回值、dst 接管 --- */
    ir_ref trd2[1] = { d0 };
    ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, trd2);
    ir_ref trr[1] = { ret_sc };
    ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, trr);
    if (op->tr != OPD_UNUSED) {
        ir_ref da = slot_addr(j, op->ar);
        ir_ref trd3[1] = { da };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, trd3);
        ir_STORE(da, o);
        ir_STORE(ir_ADD_A(da, ir_CONST_ADDR(TVAL_TAG)), ir_CONST_I32(T_OBJ));
    } else {
        store_pay(j, ret_sc, o);   /* 无 dst：obj 放 ret_sc 再整体 release = 净 0 */
        store_tag(j, ret_sc, ir_CONST_I32(T_OBJ));
        ir_ref tro[1] = { ret_sc };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, tro);
    }
    j->p_seq = 0;
    j->p_argc = 0;
}

/* P0-14 批2：调用序/op 回放发射（vm.c tphp_jit_opseq）——表达式已由 JIT
 * 求值；dst 由真 handler 写回 frame 槽，IR 端只做 deopt 门（P0-12 姿样：
 * IF_TRUE RETURN0 / IF_FALSE 留开；无 dst store、无 MERGE）。 */
static void lower_replay(JitC *j, uint32_t pc_start, uint32_t pc_end) {
    JIT_CTX();
    /* f0: 传 fn/slots——无帧直调链（tphp_jit_call/call_slots 不推帧）进入时
     * vm->x.frame 是祖先帧，opseq 入口据此自框化 temp VMFrame（见 vm.c）。 */
    ir_ref t[6] = { j->r_vm, ir_CONST_ADDR((uintptr_t)j->fn), j->r_slots,
                    ir_CONST_I32((int32_t)pc_start),
                    ir_CONST_I32((int32_t)pc_end), j->r_call_clo };
    ir_ref ok = ir_CALL_N(IR_I32,
                          ir_CONST_ADDR((uintptr_t)tphp_jit_opseq), 6, t);
    ir_ref ifs = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifs);
        ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
    ir_IF_FALSE(ifs);
    j->p_seq = 0;
}

/* finish a folded call at DO_FCALL (args already recorded in j->p_*) */
static void lower_call_finish(JitC *j, TOp *doop) {
    JIT_CTX();
    uint32_t argc = j->p_argc;
    if (j->p_bfn) {
        /* builtin call: argv scratch copy by value (slots own the refs) */
        ir_ref args_sc = j->r_call_args;
        for (uint32_t k = 0; k < argc; k++)
            arg_copy(j, ir_ADD_A(args_sc, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ)), k);
        ir_ref ret_sc = j->r_call_ret;
        ir_ref t[5] = { j->r_vm, ir_CONST_ADDR((uintptr_t)j->p_bfn),
                        ir_CONST_I32((int32_t)argc), args_sc, ret_sc };
        ir_ref ok = ir_CALL_N(IR_I32, ir_CONST_ADDR((uintptr_t)tphp_jit_bcall), 5, t);
        /* raise -> bcall returns 0: leave the JIT immediately (IR frames
         * carry no Windows UNWIND_INFO — the interpreter re-raises from C
         * frames at op_do_fcall's deopt point). Idiom per ir-master
         * examples/0008: IF_TRUE { RETURN } IF_FALSE { ... } — both sides
         * terminate on their own; NO END/MERGE after an in-branch RETURN
         * (_ir_RETURN kills control; a following END emits ref 0 and
         * ir_check reports "missing reference"). The false side stays
         * open: emit_seq continues the main sequence inside it until the
         * function-tail RETURN terminates the whole chain. */
        ir_ref ifx = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
        ir_IF_TRUE(ifx);
            ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));   /* deopt: interpreter unwinds */
        ir_IF_FALSE(ifx);
            if (doop->tr != OPD_UNUSED) {
                ir_ref da = slot_addr(j, doop->ar);
                if (dst_gate_needed(j, doop))
                    rel_dst_gate(j, da);   /* 刀2 释放门：结果覆盖旧 dst，
                        * 镜像 op_do_fcall 的 tval_release(d); d=r; */
                store_pay(j, da, ir_LOAD_I64(ret_sc));
                store_tag(j, da, ir_LOAD_I32(ir_ADD_A(ret_sc, ir_CONST_ADDR(TVAL_TAG))));
            }
        j->p_bfn = NULL;
        return;
    }
    if (j->p_dyn) {
        /* INIT_DYN 动态调用（$f(...)）：闭包值在 p_dyn_slot。
         * Uses pre-allocated entry-block scratch (not loop-body ALLOCA). */
        ir_ref args_arr = j->r_call_args;
        for (uint32_t k = 0; k < argc; k++)
            arg_copy(j, ir_ADD_A(args_arr, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ)), k);
        ir_ref clo_sc = j->r_call_clo;
        ir_ref ca = slot_addr(j, j->p_dyn_slot);
        store_pay(j, clo_sc, load_pay(j, ca));
        store_tag(j, clo_sc, load_tag(j, ca));
        ir_ref ret_sc = j->r_call_ret;
        /* P0-15 B: closure direct-call fast path (bug1 fix: LOAD_A(clo_sc)
         * not double-deref; bug2 fix: is_method excluded).
         * 批2: 门2 per-site fn 快照——clo->fn == 该 site 上次链通的 fn 即免
         * if2..if7 全链（fn 身份蕴含 bfn==0/fn!=0/n_cap==0/entry!=0 写一次
         * 不清/nslots/is_method，TFunc 不可变，推导见 PLAN spec）。 */
        ir_ref slow[7]; uint32_t nslow = 0;
        ir_ref if1 = ir_IF(ir_NE(load_tag(j, clo_sc), ir_CONST_I32(T_CLOSURE)));
        ir_IF_TRUE(if1); slow[nslow++] = ir_END(); ir_IF_FALSE(if1);
        ir_ref c = ir_LOAD_A(clo_sc);
        ir_ref fn = ir_LOAD_A(ir_ADD_A(c,
            ir_CONST_ADDR(offsetof(TClosure, fn))));
        ir_ref snap = ir_LOAD_A(ir_CONST_ADDR(j->p_dyn_ic));
        ir_ref ifg = ir_IF(ir_EQ(fn, snap));
        ir_IF_TRUE(ifg);
            ir_ref e_hit = ir_END();   /* 快照命中：免链，直用 fn->jit_entry */
        ir_IF_FALSE(ifg);
        ir_ref if2 = ir_IF(ir_NE(ir_LOAD_I64(ir_ADD_A(c,
            ir_CONST_ADDR(offsetof(TClosure, bfn)))), ir_CONST_I64(0)));
        ir_IF_TRUE(if2); slow[nslow++] = ir_END(); ir_IF_FALSE(if2);
        ir_ref if3 = ir_IF(ir_EQ(fn, ir_CONST_I64(0)));
        ir_IF_TRUE(if3); slow[nslow++] = ir_END(); ir_IF_FALSE(if3);
        ir_ref if4 = ir_IF(ir_NE(ir_LOAD_U16(ir_ADD_A(c,
            ir_CONST_ADDR(offsetof(TClosure, n_cap)))), ir_CONST_I32(0)));
        ir_IF_TRUE(if4); slow[nslow++] = ir_END(); ir_IF_FALSE(if4);
        ir_ref entry_chk = ir_LOAD_I64(ir_ADD_A(fn,
            ir_CONST_ADDR(offsetof(TFunc, jit_entry))));
        ir_ref if5 = ir_IF(ir_EQ(entry_chk, ir_CONST_I64(0)));
        ir_IF_TRUE(if5); slow[nslow++] = ir_END(); ir_IF_FALSE(if5);
        ir_ref nsl = ir_ADD(IR_I32,
            ir_ZEXT(IR_I32, ir_LOAD_U16(ir_ADD_A(fn,
                ir_CONST_ADDR(offsetof(TFunc, n_locals))))),
            ir_ZEXT(IR_I32, ir_LOAD_U16(ir_ADD_A(fn,
                ir_CONST_ADDR(offsetof(TFunc, n_tmps))))));
        ir_ref if6 = ir_IF(ir_GT(nsl, ir_CONST_I32(64)));
        ir_IF_TRUE(if6); slow[nslow++] = ir_END(); ir_IF_FALSE(if6);
        ir_ref if7 = ir_IF(ir_NE(ir_LOAD_U8(ir_ADD_A(fn,
            ir_CONST_ADDR(offsetof(TFunc, is_method)))), ir_CONST_I32(0)));
        ir_IF_TRUE(if7); slow[nslow++] = ir_END(); ir_IF_FALSE(if7);
        /* 批2: 链全过 → 填 per-site 快照（单字对齐指针原子写；跨线程最坏
         * 读到旧 fn → 门2 miss 走链，无撕裂面） */
        ir_STORE(ir_CONST_ADDR(j->p_dyn_ic), fn);
        ir_ref e_chain = ir_END();
        ir_MERGE_2(e_chain, e_hit);
        /* 命中支 fn==snap 为真 fn（ic 初值 1，链尾只存过 if5 校验的真 fn），
         * 链支过了 if3——两支 fn 均非 NULL，entry 现读安全且恒非 0 */
        ir_ref entry = ir_LOAD_I64(ir_ADD_A(fn,
            ir_CONST_ADDR(offsetof(TFunc, jit_entry))));
        ir_ref slots = j->r_call_slots;
        for (uint32_t k = 0; k < argc; k++) {
            ir_ref sa = ir_ADD_A(args_arr, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
            ir_ref d = ir_ADD_A(slots, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
            store_pay(j, d, ir_LOAD_I64(sa));
            store_tag(j, d, ir_LOAD_I32(ir_ADD_A(sa, ir_CONST_ADDR(TVAL_TAG))));
        }
        /* （非参数槽的 UNDEF 保护在 callee 侧只填 CV 区——见
         * tphp_jit_compile 的 prologue 注；此地曾试填满 64 上界，
         * 实测 126 store/调用把 P0-15 收益全吃掉 kv 39→53 已撤） */
        ir_ref dir_t[3] = { j->r_vm, slots, ret_sc };
        ir_ref ok = ir_CALL_N(IR_I32, entry, 3, dir_t);
        ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
        ir_IF_TRUE(ifd);
            ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));   /* P1-12 批2-2 */
        ir_IF_FALSE(ifd);
        ir_ref efast = ir_END();
        ir_MERGE_N((ir_ref)nslow, slow);
        ir_ref t5[5] = { j->r_vm, clo_sc, args_arr,
                         ir_CONST_I32((int32_t)argc), ret_sc };
        ir_ref ok2 = ir_CALL_N(IR_I32,
                               ir_CONST_ADDR((uintptr_t)tphp_jit_call_dyn), 5, t5);
        ir_ref ifd2 = ir_IF(ir_EQ(ok2, ir_CONST_I32(0)));
        ir_IF_TRUE(ifd2);
            ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));   /* P1-12 批2-2 */
        ir_IF_FALSE(ifd2);
        ir_ref eslow = ir_END();
        ir_MERGE_2(eslow, efast);
        if (doop->tr != OPD_UNUSED) {
            ir_ref da = slot_addr(j, doop->ar);
            if (dst_gate_needed(j, doop))
                rel_dst_gate(j, da);   /* 刀2 释放门（同 bcall：结果覆盖旧
                                        * dst；刀3 槽级免发除外） */
            store_pay(j, da, ir_LOAD_I64(ret_sc));
            store_tag(j, da, ir_LOAD_I32(ir_ADD_A(ret_sc, ir_CONST_ADDR(TVAL_TAG))));
        }
        j->p_dyn = 0;
        j->p_dyn_ic = 0;
        return;
    }
    TFunc *callee = j->p_callee;
    if (!callee || argc != callee->n_params) { j->fail = 1; if (!j->fail_line) j->fail_line = __LINE__; return; }
    /* callee 编译失败（hot=UINT16_MAX, entry NULL）→ caller 拒编走解释器：
     * 否则运行时 call_slots 返 0 读垃圾 ret（修复既有隐患，compile_callees
     * 只尝试不保证成功）。self 递归跳过（执行时自身 entry 必已设）。 */
    if (callee != j->fn && !callee->jit_entry) { j->fail = 1; if (!j->fail_line) j->fail_line = __LINE__; return; }
    /* 批3-3：静态方法直调放行（p_sm=eligibility 已解析 user-static 且
     * 可见性/形态全验）；批2-4：p_mcall = INIT_MCALL 折叠直调（三守卫已
     * 证实单态 user 实例方法）；其余 is_method TFunc 仍拒（批2-2 前行为）。 */
    if (callee->is_method && !j->p_sm && !j->p_mcall) { j->fail = 1; if (!j->fail_line) j->fail_line = __LINE__; return; }
    /* 批2-7: 函数级 mcall 三道运行时守卫（镜像 OSR 在 INIT_MCALL 的版本，
     * 读 r_call_clo 快照）——此处实参表达式已由 IR 无条件求值完：失配
     * END 收集（mfail），OP_DO_FCALL 聚合后 lower_replay 只重跑调用序
     * （opseq 跳过表达式 op 恰正确）；RETURN 0 会从 pc 0 重启整函数
     * （序前副作用双执行）。OSR 守卫仍在 INIT（RETURN 语义，实参未求值）。 */
    bool mf = j->p_mcall && !j->osr;
    if (mf) {
        ir_ref gs = j->r_call_clo;
        ir_ref g1 = ir_IF(ir_NE(load_tag(j, gs), ir_CONST_I32(T_OBJ)));
        ir_IF_TRUE(g1);
            j->mfail[j->mfail_n++] = ir_END();
        ir_IF_FALSE(g1);
        ir_ref ob = ir_LOAD_A(gs);
        ir_ref g2 = ir_IF(ir_NE(
            ir_LOAD_U8(ir_ADD_A(ob, ir_CONST_ADDR(offsetof(GCHead, kind)))),
            ir_CONST_U8(GC_KIND_OBJ)));
        ir_IF_TRUE(g2);
            j->mfail[j->mfail_n++] = ir_END();
        ir_IF_FALSE(g2);
        ir_ref g3 = ir_IF(ir_NE(
            ir_LOAD_A(ir_ADD_A(ob, ir_CONST_ADDR(offsetof(TObj, cls)))),
            ir_CONST_ADDR((uintptr_t)j->p_mcall_cls)));
        ir_IF_TRUE(g3);
            j->mfail[j->mfail_n++] = ir_END();
        ir_IF_FALSE(g3);
    }
    /* 方法帧布局：slot0 保留（$this 槽，静态不可达）、参数 1..（compile.c
     * get_var("this") 对全部方法预留 CV0）——函数布局 poff=0。 */
    uint32_t poff = (j->p_sm || j->p_mcall) ? 1u : 0u;

    /* v1.2 direct-slot call: pre-allocated entry-block scratch, write params
     * in place, call entry via runtime lookup — no TLS, no loop ALLOCA.
     * Small callees (≤9 slots) reuse the args buffer as slots. */
    uint32_t nslots = (uint32_t)callee->n_locals + callee->n_tmps;
    ir_ref slots_sc;
    if (nslots <= 9) {
        slots_sc = j->r_call_args;
    } else {
        slots_sc = ir_ALLOCA(ir_CONST_I64((uintptr_t)nslots * TVAL_SZ));
    }
    /* init slot0(方法)/locals/tmps to UNDEF (params get overwritten below)
     * —— p_mcall 的 slot0=$this 在合同检查后填（addref 前无早退路径） */
    if (poff && !j->p_mcall) {
        ir_ref d0 = ir_ADD_A(slots_sc, ir_CONST_ADDR(0));
        store_imm(j, d0, (int32_t)T_UNDEF, ir_CONST_I64(0));
    }
    for (uint32_t k = poff + callee->n_params; k < nslots; k++) {
        ir_ref d = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
        store_imm(j, d, (int32_t)T_UNDEF, ir_CONST_I64(0));
    }
    for (uint32_t k = 0; k < argc; k++) {
        ir_ref d = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)(poff + k) * TVAL_SZ));
        arg_copy(j, d, k);
    }
    ir_ref ret_sc = j->r_call_ret;
    /* P0-15 A: static direct call - runtime entry load + indirect CALL;
     * INT contract per-param IF chain, failure -> RETURN 0 (same semantics
     * as call_slots returning 0: callee not executed, caller deopts). */
    ir_ref entry = ir_LOAD_I64(ir_ADD_A(ir_CONST_ADDR((uintptr_t)callee),
                                        ir_CONST_ADDR(offsetof(TFunc, jit_entry))));
    ir_ref ife = ir_IF(ir_EQ(entry, ir_CONST_I64(0)));
    ir_IF_TRUE(ife);
        /* 批2-7: 函数级 mcall → mfail（callee 未执行，回放重跑调用序）；
         * 其余保持 RETURN（P1-12 批2-2 序起点重跑） */
        if (mf) j->mfail[j->mfail_n++] = ir_END();
        else ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));
    ir_IF_FALSE(ife);
    /* 刀19（11-09）+刀20：非 INT 实参慢路收集——普通函数与 p_sm 静态方法
     * （契约失配不再 RETURN0 = 函数级 pc0 全量重跑 → 序前副作用双执行，
     * 实证 AAB5≠AB5 与 SMK::len 双写；刀18 real-logstat +47% 真凶），改走
     * tphp_jit_call_userfn 慢路（合成捕获闭包 → guarded = op_do_fcall C 侧
     * 同协议，callee 单次执行；p_sm 侧 helper 自判 is_method 合成 n_cap=1
     * 垫闭包对位 slot1..，emit 传参基址同步 +TVAL_SZ）。
     * p_mcall（实例方法直调，mf=mfail 回放）保持既有路径；TPHP_CALL_STR_SLOW=0
     * 关（HEAD 行为，ncslow 恒 0 → IR 与 HEAD 逐位同）。argc≤32 防御。 */
    ir_ref cslow[32];
    uint32_t ncslow = 0;
    int str_slow = call_str_slow_enabled() && !mf && !j->p_mcall;
    for (uint32_t k = 0; k < argc; k++) {
        ir_ref a = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)(poff + k) * TVAL_SZ));
        ir_ref tg = ir_LOAD_I32(ir_ADD_A(a, ir_CONST_ADDR(TVAL_TAG)));
        ir_ref ift = ir_IF(ir_NE(tg, ir_CONST_I32(T_INT)));
        ir_IF_TRUE(ift);
            /* 批2-7: 函数级 mcall → mfail（非 INT 契约失配，callee 未执行；
             * arg_copy 是 raw 拷贝无 addref，弃 scratch 零泄漏） */
            if (mf) j->mfail[j->mfail_n++] = ir_END();
            else if (str_slow && ncslow < 32) cslow[ncslow++] = ir_END();
            else ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));   /* P1-12 批2-2 */
        ir_IF_FALSE(ift);
    }
    ir_ref mcall_d0 = IR_UNUSED;
    if (j->p_mcall) {
        /* 批2-4：$this@slot0 = 接收者快照 raw copy + addref（T_OBJ/kind/
         * cls 三守卫已在 INIT 证实；合同/entry 早退均在本 addref 之前）。
         * callee 无帧执行，调用后统一 release 归还：callee move 走
         * （return $this → slot0 置 UNDEF）则空操作，所有权随 ret 转移；
         * callee 复制到别处则各自 addref，此处 release 恰好配平。 */
        mcall_d0 = ir_ADD_A(slots_sc, ir_CONST_ADDR(0));
        store_pay(j, mcall_d0, load_pay(j, j->r_call_clo));
        store_tag(j, mcall_d0, load_tag(j, j->r_call_clo));
        ir_ref thiso = ir_LOAD_A(mcall_d0);
        ir_STORE(thiso, ir_ADD_I32(ir_LOAD_I32(thiso), ir_CONST_I32(1)));
    }
    ir_ref dir_t[3] = { j->r_vm, slots_sc, ret_sc };
    ir_ref ok = ir_CALL_N(IR_I32, entry, 3, dir_t);
    /* callee deopt（内含 raise，exception 已设）→ caller 同样立即 RETURN 0
     * 沿 JIT→JIT 链逐层传播，直到解释器 op_do_fcall C 帧 longjmp（不跨 IR）。
     * 批2-7: 函数级 mcall 也保持 RETURN——此处 callee 已部分执行，mfail
     * 回放会经解释器再调一次 callee（双执行 callee），RETURN 让异常链
     * 直达 op_do_fcall longjmp 才是正确语义。 */
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        if (j->p_mcall) {   /* 归还 $this addref（slot0 可能已被 callee 改写） */
            ir_ref trd[1] = { mcall_d0 };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, trd);
        }
        ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));   /* P1-12 批2-2 */
    ir_ref efast = IR_UNUSED;
    ir_IF_FALSE(ifd);
    if (ncslow) efast = ir_END();   /* 刀19: 有慢路边 → 快路止步，dst store 移至 MERGE 后 */
    if (ncslow) {
        /* 刀19/20 慢路：非 INT 实参 → tphp_jit_call_userfn（合成捕获闭包走
         * guarded）。args = slots_sc raw 借用（arg_copy 已填 [poff..poff+argc)，
         * helper 内 addref 进队配平由既有协议负责；p_sm 参数在 slot1.. →
         * 基址 +TVAL_SZ，slot0 垫片由 helper n_cap=1 闭包自补）；
         * 释放只针对本函数合成的 clo 引用（tclo rc=1）。 */
        ir_MERGE_N((ir_ref)ncslow, cslow);
        ir_ref args_pa = j->p_sm
            ? ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)TVAL_SZ))
            : slots_sc;
        ir_ref ut[5] = { j->r_vm, ir_CONST_ADDR((uintptr_t)callee), args_pa,
                         ir_CONST_I32((int32_t)argc), ret_sc };
        ir_ref ok2 = ir_CALL_N(IR_I32,
                               ir_CONST_ADDR((uintptr_t)tphp_jit_call_userfn), 5, ut);
        ir_ref ifd2 = ir_IF(ir_EQ(ok2, ir_CONST_I32(0)));
        ir_IF_TRUE(ifd2);
            ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));   /* 逃逸异常上传（同既有 RETURN 语义） */
        ir_IF_FALSE(ifd2);
        ir_ref eslow = ir_END();
        ir_MERGE_2(eslow, efast);
    }
    if (doop->tr != OPD_UNUSED) {
        ir_ref da = slot_addr(j, doop->ar);
        if (dst_gate_needed(j, doop))
            rel_dst_gate(j, da);   /* 刀2 释放门（同 bcall：结果覆盖旧 dst；
                                    * 刀3 槽级免发除外） */
        store_pay(j, da, ir_LOAD_I64(ret_sc));
        store_tag(j, da, ir_LOAD_I32(ir_ADD_A(ret_sc,
                                              ir_CONST_ADDR(TVAL_TAG))));
    }
    if (j->p_mcall) {
        ir_ref trd2[1] = { mcall_d0 };   /* 成功路径同样配平（UNDEF 空操作） */
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, trd2);
    }
    j->p_callee = NULL;
    j->p_sm = 0;
    j->p_mcall = 0;
    j->p_mcall_cls = NULL;
}

static void lower_return(JitC *j, TOp *op) {
    JIT_CTX();
    if (op->t1 != OPD_UNUSED) {
        ir_ref ta = opnd_tag(j, op->t1, op->a1);
        ir_ref pa = opnd_pay(j, op->t1, op->a1);
        /* move semantics: raw-copy to ret, clear source slot. frame_destroy
         * sees UNDEF (no double-free); JIT→JIT caller owns the ref. */
        store_pay(j, j->r_ret, pa);
        store_tag(j, j->r_ret, ta);
        if (op->t1 == OPD_CV || op->t1 == OPD_TMP) {
            ir_ref sa = slot_addr(j, op->a1);
            store_imm(j, sa, (int32_t)T_UNDEF, ir_CONST_I64(0));
        }
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
                /* P0-14 批5: 回跳=continue（跳出 then 至循环头）——不参与
                 * join 判定，继续扫 jends 定 join；前跳短于 T 仍拒 */
                if (op->ar <= q) break;
                if (op->ar < T) return 0;
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

/* P0-14 批5: || 短路（JMPNZ→真值区 [CONST true; JMP jend]）的 find_join
 * 深扫会越过 jend 错位。直取 jend 方案（find_or_join）实测 compile 过但
 * 值错（ms2 族）——已撤；现行为=深扫下 tB END@jend≠join → 自然 fail →
 * 回退解释器（语义安全）。|| 进 JIT = 独立户：真值区/jend/合流需整体
 * 重设计（backlog P0-14 余量）。JMPNZ 极性修正（ifx=truthy）保留打底。 */

static int is_backedge_target(TFunc *fn, uint32_t pc) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if ((op->op == OP_JMP || op->op == OP_JMPZ || op->op == OP_JMPNZ) &&
            op->ar == pc && pc <= i)
            return 1;
    }
    return 0;
}

/* P0-19 round-5 折叠门（emit-ssa.md §7 去留条款）：折叠 fast 线在本发射
 * 器上净负收益（base 9.7 vs B' 10.8 vs B'+C 11.0，array-read 1e6 5×5 交
 * 替），默认关断，TPHP_FOLD_ADD=1 重开。循环头 acc phi 与折叠检测共用
 * 本开关——acc phi 只服务折叠快线的 reconcile；fold 关断时创建 phi 只会
 * 产生"回边补丁恒 0"的死 phi（PHI ops[3]=0 → ir_check missing reference，
 * 11-01 的 "232" 由此而来，11-02 根因定位修正：非 deps/ir 上游缺陷）。 */
static int fold_add_enabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("TPHP_FOLD_ADD") ? 1 : 0;
    return v;
}

/* 刀A拆税子实验（2026-11-05）：TPHP_FOLD_ACC=0 只关批C acc loop-phi
 *（2848 fold_acc_scan→0，fwd 汇合走非 phi 的普通 MERGE = B' 骨架保留），
 * 与 TPHP_FOLD_ADD=0（纯默认）三态对照拆 1.3ns 税归属（B' 结构 vs C phi）。
 * 默认 1 = 与 FOLD_ADD 同效（现状 B'+C）。 */
static int fold_acc_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_FOLD_ACC");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* P0-append 快线开关（11-04 新地图 array-push≈3.5×=杠杆第一）：默认开，
 * TPHP_APPEND_FAST=0 关（同二进制 A/B）。 */
static int append_fast_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_APPEND_FAST");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 刀4（11-05 keyed 覆盖写快路）开关：默认开，TPHP_SETDIM_FAST=0 关
 * （同二进制 A/B，append 快路同款先例）。 */
static int setdim_fast_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_SETDIM_FAST");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 类型特化批1（PLAN/type-spec.md §5 批1）开关：默认开，
 * TPHP_TYPE_SPEC=0 关（同二进制 A/B）。关 = 无 guard 无检查点跳过
 * = HEAD 行为。 */
static int type_spec_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_TYPE_SPEC");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 刀15（11-09 take 瘦身）开关：默认开，TPHP_TAKE_THIN=0 关（同二进制
 * A/B）。关 = dimic_take 原三门 + 无 preheader 消灭 = HEAD 行为。
 * 开关同时控制 preheader 消灭与 take thin 判定（spec_body_tmp 非零
 * 仅在开时）——两者互为依据，单一真源防错配。 */
static int take_thin_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_TAKE_THIN");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 刀16（11-09 FETCH 值直达）开关：默认开，TPHP_FETCH_FWD=0 关（同二进制
 * A/B）。关 = 回刀15 thin（take 16B store + ADD 槽 load 照旧）。
 * 仅在 thin 条件（spec_c+spec_body_tmp 身份）之上叠加生效。 */
static int fetch_fwd_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_FETCH_FWD");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 批4b-i（key-fwd）开关：默认开，TPHP_KEY_FWD=0 关（同二进制 A/B）。
 * 关 = F1 键槽 take 照旧 + F2 键位槽 load 照旧（= 批4a 行为）。 */
static int key_fwd_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_KEY_FWD");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 批4b-ii（链式 fwd，§8.6）开关：默认开，TPHP_CHAIN_FWD=0 关（同二进制
 * A/B）。关 = spec_scan 链式模式不放行（cv8/const 链式形态照拒 = 批4b-i
 * 行为），只闸 scan 放行一处，关时全链零 IR 变化。 */
static int chain_fwd_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_CHAIN_FWD");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 批4c（单 fetch ht 化，§8.6）开关：默认开，TPHP_ONEKEY=0 关（同二进制
 * A/B）。关 = spec_scan 1key/STR-lit 分流不放行（键≠idx 单 fetch 照拒 =
 * 今日行为），只闸分流一处，关时零 IR 变化；packed read 分流不受本开关
 * 影响（键==idx 判据先于本分流）。 */
static int onekey_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_ONEKEY");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 刀19（11-09 静态直调非 INT 慢路）开关：默认开，TPHP_CALL_STR_SLOW=0 关
 * （同二进制 A/B）= HEAD 行为（INT 契约失配 RETURN0 → pc0 全量重跑）。 */
static int call_str_slow_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_CALL_STR_SLOW");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* logstat 百菜第一点（11-10）：MOD int 快线开关——默认开，TPHP_MOD_FAST=0
 * 关（同二进制 A/B）= HEAD 行为（恒 CALL tval_mod）。 */
static int mod_fast_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_MOD_FAST");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* object 刀（11-13）：NEG int 快线开关——默认开，TPHP_NEG_FAST=0 关（同
 * 二进制 A/B）= HEAD 行为（恒 CALL tval_neg 慢线）。 */
static int neg_fast_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_NEG_FAST");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 刀18（11-09 logstat 勘察）：有效急切 ops 门，env 单次解析（jit.h 注释
 * 有结论：默认 96=HEAD，扩容实测反亏不改默认）。0 = 关急切（仅 hot127）。 */
int tphp_jit_eager_max_ops(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_JIT_EAGER_OPS");
        if (e && e[0]) { v = atoi(e); if (v < 0) v = 0; }
        else v = TPHP_JIT_EAGER_DEFAULT_OPS;
    }
    return v;
}

/* 刀5（11-05 FETCH_PROP helper 化）开关：默认开，TPHP_PROP_HELPER=0 关
 * = 回到单 op 回放 + frameless 拒 FETCH_PROP（HEAD 行为）。两处必须同开
 * 同关——helper 关而 frameless 放开 = 无帧跑回放必错。 */
static int prop_helper_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_PROP_HELPER");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* prop v1.5 批1（PLAN/prop-ic.md）开关：默认开，TPHP_PROP_INLINE=0 关
 * = 回 HEAD helper 路径（同二进制 A/B）。仅 CONST 名生效；动态名恒慢路。 */
static int prop_inline_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_PROP_INLINE");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;
    }
    return v;
}

/* 刀5C（11-06）嵌套调用 emit 开关：**默认开**（11-06 用户拍板）。
 * 历史：曾默认关——A/B 实测 bench() 计时壳 JIT 化后 call_dyn 使被测
 * 闭包未预编译 → 循环中途 OSR 编译开销落进计时区 → 全表前段虚高。
 * 刀9 补齐 callee eager 门后重开条件②满足，用户拍板默认开。
 * TPHP_NEST_CALL=0 可关。功能本身：n1/n2/n3 不再 FAILED、长循环+嵌套
 * 调用专项 −14.8%、四模语义一致。 */
static int nest_call_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_NEST_CALL");
        v = (e && e[0] == '0') ? 0 : 1;   /* 默认开，=0 关 */
    }
    return v;
}

/* P0-17③ 批C slice-1：canonical 折叠累加形态守卫（loop phi 激活前提）。
 * 循环体（cond JMPZ 之后 → 回边 JMP）必须严格匹配：
 *   - 恰一个 FETCH_DIM_R+ADD 对，且 ADD 为 RMW 直写 CV（t?==OPD_CV &&
 *     a?==tr，即 $s += $a[$i] 两 op 形态，镜像 emit 侧配对检测）；
 *   - 其余 op 仅限 {INC, CONST}，且不得以任何操作数位触碰 acc 槽；
 *   - 体尾存在 JMP→hdr。
 * 任何结构外 op（嵌套 if/循环/调用/prop 等）→ 返回 0（回退 B' 全 store）。
 * 守卫通过即保证：体无嵌套控制流 → fwd merge 恒达、reconcile phi 恒
 * 支配回边、内层循环 pop 不会清掉本层激活态。 */
static uint32_t fold_acc_scan(TFunc *fn, uint32_t hdr) {
    uint32_t acc = 0, fetch_pc = 0, add_pc = 0;
    int seen_fetch = 0;
    uint32_t i = hdr;
    while (i < fn->n_ops && fn->ops[i].op != OP_JMPZ &&
           fn->ops[i].op != OP_JMPNZ) i++;
    if (i >= fn->n_ops) return 0;
    i++;                                    /* 体起点 = cond JMPZ + 1 */
    for (; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if (op->op == OP_JMP) {
            if (op->ar == hdr) return acc;  /* 回边：守卫通过 */
            return 0;                       /* 前跳（break/goto 形态） */
        }
        switch (op->op) {
        case OP_FETCH_DIM_R:
            if (seen_fetch || i + 1 >= fn->n_ops) return 0;
            seen_fetch = 1; fetch_pc = i;
            break;
        case OP_ADD: {
            if (!seen_fetch || add_pc) return 0;
            TOp *f = &fn->ops[fetch_pc];
            /* 折叠配对（镜像 emit 侧检测：kind 对 kind、idx 对 idx）
             * + RMW 限定（dst 为 CV 且 dst==src，即 $s += 形态） */
            int pair = (op->t2 == f->tr && op->a2 == f->ar &&
                        op->tr == OPD_CV && op->t1 == OPD_CV &&
                        op->a1 == op->ar) ||
                       (op->t1 == f->tr && op->a1 == f->ar &&
                        op->tr == OPD_CV && op->t2 == OPD_CV &&
                        op->a2 == op->ar);
            if (!pair) return 0;
            add_pc = i; acc = op->ar;       /* RMW：dst(ar) 即累加器 */
            break;
        }
        case OP_INC:
        case OP_CONST:
            break;
        default:
            return 0;
        }
        if (acc && i != add_pc) {           /* 折叠 ADD 之外触碰 acc → 不激活 */
            if ((op->t1 == OPD_CV || op->t1 == OPD_TMP) && op->a1 == acc) return 0;
            if ((op->t2 == OPD_CV || op->t2 == OPD_TMP) && op->a2 == acc) return 0;
            if (op->tr == OPD_CV && op->ar == acc) return 0;
        }
    }
    return 0;                               /* 体尾无回边 */
}

/* ==== POC array-read preguard（memory/2026-11-04 §1 路线甲） ==== */

static int preguard_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("TPHP_PREGUARD");
        v = (!e || !e[0] || e[0] == '1') ? 1 : 0;  /* 默认开；TPHP_PREGUARD=0 关（A/B） */
    }
    return v;
}

/* 体内白名单：仅"读任意 + 写自身标量槽"的纯 op（emit 层语义已审计：
 * 无调用、无数组变更、无 RETURN/THROW/FE）。白名单外任何 op → 整循环
 * 放弃外提。tphp 无引用类型（无 T_REF/OP_REF）→ 槽写即全部可变面，
 * 不存在别名逃逸改写 base 的路径。 */
static int pre_pure_op(const TOp *op) {
    switch (op->op) {
    case OP_CONST: case OP_COPY:
    case OP_ADD: case OP_SUB:
    case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE:
    case OP_LT: case OP_LE: case OP_CMP:
    case OP_JMP: case OP_JMPZ: case OP_JMPNZ:
    case OP_BOOL_VAL:                     /* make_truthy 纯读 + 写 tmp（循环
                                           * cond 恒发 BOOL_VAL→JMPZ 形态）*/
    case OP_FETCH_DIM_R:                 /* 只读 base（helper/IC 亦只读） */
    case OP_INC: case OP_DEC:
        return 1;
    default:
        return 0;
    }
}

/* op 对槽 s 的写点（镜像各 lower 的写入位置；无 dst 判别的 op 按 ar
 * 保守恒算写——只多取消候选，不会漏取消）。跳转族 ar=目标 pc，非槽写。 */
static int pre_op_writes(const TOp *op, uint32_t s) {
    switch (op->op) {
    case OP_JMP: case OP_JMPZ: case OP_JMPNZ:
        return 0;
    case OP_INC: case OP_DEC:
        return op->a1 == s || op->ar == s;   /* RMW 恒写 a1，有 dst 另写 ar */
    default:
        return op->ar == s;
    }
}

/* 循环 [hdr..be] 不变量候选扫描：单回边、单前驱入口、区域白名单、
 * 体内对 base 槽零 store。返回候选槽数（0 = 不做外提）。 */
static int preguard_scan(TFunc *fn, uint32_t hdr, uint32_t *out, int nmax) {
    const char *why = NULL;         /* 拒因（jit_dbg 常开风格同 osr reject）*/
    uint32_t badpc = hdr;
    if (hdr >= fn->n_ops || nmax <= 0) return 0;
    /* 单回边：多回边形态（if 内 continue ≥2 回跳头）先不做 */
    uint32_t be = 0;
    int nb = 0;
    for (uint32_t i = hdr + 1; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if (op->op == OP_JMP && op->ar == hdr) { nb++; be = i; }
    }
    if (nb != 1) { why = "not single backedge"; goto out; }
    /* 单前驱入口：fall-through + pc<hdr 跳入 hdr 合计 ≤1（多入口时
     * 前驱块可能不经外提菱形 → 支配失败）；外部跳入体内 [hdr,be] → 放弃 */
    int fpred = 0;
    if (hdr > 0) {
        TOp *pv = &fn->ops[hdr - 1];
        if (pv->op != OP_JMP && pv->op != OP_JMPZ && pv->op != OP_JMPNZ &&
            pv->op != OP_RETURN)
            fpred = 1;                      /* 已知非落空的控制 op 之外一律
                                             * 按落空计（多计只会多放弃=安全）*/
    }
    for (uint32_t i = 0; i < hdr; i++) {
        TOp *op = &fn->ops[i];
        if (op->op != OP_JMP && op->op != OP_JMPZ && op->op != OP_JMPNZ)
            continue;
        if (op->ar == hdr) fpred++;
        else if (op->ar > hdr && op->ar <= be) {
            why = "ext jump into body"; badpc = i; goto out;
        }
    }
    for (uint32_t i = be + 1; i < fn->n_ops; i++) {   /* 体后跳入头/体内 */
        TOp *op = &fn->ops[i];
        if ((op->op == OP_JMP || op->op == OP_JMPZ || op->op == OP_JMPNZ) &&
            op->ar >= hdr && op->ar <= be) {
            why = "post jump into body"; badpc = i; goto out;
        }
    }
    if (fpred > 1) { why = "multi forward pred"; goto out; }
    /* 区域白名单 + 候选 base 收集 */
    uint32_t cand[8];
    int nc = 0;
    for (uint32_t i = hdr; i <= be; i++) {
        TOp *op = &fn->ops[i];
        if (!pre_pure_op(op)) {
            why = tphp_op_name(op->op); badpc = i; goto out;
        }
        if (op->op == OP_JMP || op->op == OP_JMPZ || op->op == OP_JMPNZ) {
            if (op->ar < hdr) { why = "backward out of region"; badpc = i; goto out; }
            /* ar==hdr=唯一回边（nb==1 已证）；ar∈(hdr,be]=内层回边头在
             * 区域内（嵌套循环，其自身 push 时再扫）；ar>be=正向出口 */
        }
        if (op->op == OP_FETCH_DIM_R &&
            (op->t1 == OPD_CV || op->t1 == OPD_TMP)) {
            int seen = 0;
            for (int k = 0; k < nc; k++) if (cand[k] == op->a1) seen = 1;
            if (!seen && nc < 8) cand[nc++] = op->a1;
        }
    }
    /* 写点过滤：体内对 base 槽零 store 才可外提 */
    int out_n = 0;
    for (int k = 0; k < nc; k++) {
        int wr = 0;
        for (uint32_t i = hdr; i <= be && !wr; i++)
            if (pre_op_writes(&fn->ops[i], cand[k])) wr = 1;
        if (!wr && out_n < nmax) out[out_n++] = cand[k];
    }
    return out_n;
out:
    if (why && jit_dbg())
        fprintf(stderr, "[jit] preguard reject hdr=%u pc=%u: %s\n",
                hdr, badpc, why);
    return 0;
}

/* 体内 FETCH 查本层（最内活跃循环）候选：-1 = 无外提，走原装载 */
static int preguard_lookup(JitC *j, uint32_t slot) {
    if (j->loop_sp <= 0) return -1;
    int k = j->loop_sp - 1;
    for (int i = 0; i < j->pre_n[k]; i++)
        if (j->pre_slot[k][i] == slot) return i;
    return -1;
}

/* ==== 类型特化批1（PLAN/type-spec.md §5 批1）==== */
/* read 形态循环守卫：结构证明面（单回边/单前驱/无外跳/纯 op 白名单/
 * base 体内零写）整段复用 preguard_scan——判据一处维护；叠加形态判定：
 *   [cond 段 hdr..JMPZ] 不扫（emit 照原样，无检查点依赖其形态）；
 *   [体 JMPZ+1..be) 恰一个 FETCH_DIM_R（key=槽）+ 紧邻恰一个 ADD
 *   （RMW 直写 CV：dst==src 之一，另一 src 恰为 FETCH dst）+ 恰一个
 *   INC/DEC（a1==key 槽），其余 op 一律拒，回边已在 be；
 *   写点：除配对 ADD（写 acc）与 INC/DEC（写 idx）外，体内任何 op
 *   不得写 acc/idx；任何 op 不得写 base（preguard 已证，双保险）。
 * 输出 acc（返回 0 = 不特化）/ idx / tmp / base / be。
 * 返回 1 = 特化；acc 经出参——槽号 0 合法（无参闭包首 CV），不可兼作
 * 成功标志。
 * 批4a（§8.3-1，kv 形态）：恰 2 个 FETCH 放行——F1（键取值，key=CV idx、
 * dst=键 TMP）紧邻 F2（值查表，base=CV、key=F1 dst）、ADD 紧邻 F2；
 * INC 对 F1 的 key 槽（ix）。身份扩键 TMP/键 base 两元，写点约束扩
 * key_base。form：nfetch==2 ⇒ ht（guard 走 tphp_jit_spec_guard_ht，
 * $m 为 hash 键基；packed 基的 2-fetch 形态 guard 失败交解释器=设计
 * 记账的回退面）。
 * 批4b-ii（§8.6 链式 fwd）：F2 键为 CV/CONST（kv F2 键恒 TMP=F1 dst）⇒
 * 分叉进链式模式——N fetch（2≤N≤16，全部同 base CV、键 CV/CONST、dst
 * TMP）+ N−1 链加（dst TMP，src 配对 = 前链 dst/首链 = F1F2 dst + 本
 * fetch dst）+ 1 RMW 收尾（dst CV=acc，另一 src = 末链 dst）+ 1 INC，
 * 严格交替 F F (A_c F)* A_c A_rmw，其余一律拒。身份 = CV 集
 * {acc,ix,base,键 CV}（键彼此可重复、不得撞前三；TMP 域经 relocate
 * 与 CV 不交、tmp_new 单调 ⇒ TMP 间恒异）。写点约束扩全部键 CV。
 * form：链式 ⇒ ht=1（guard_ht 单 base）+ chain=1 + tmp=0（不吃
 * preheader 消灭——物化自带释放门，见 ctab_mat_all）。
 * 批4c（§8.6 单 fetch ht 化）：单 fetch 且 INC 目标≠键槽（nfetch==1、
 * nadd==1）⇒ onekey 形态分流——键==idx 者恒走 read 路径（判据原样、
 * ht=0）；onekey ⇒ ht=1 + kstr（1=CV 键，guard 合取键槽 tag==STR——
 * 键循环期不写由写点约束证；2=STR 字面量键，编译期已证 guard 免查；
 * 非 STR 字面量收尾拒 = 保今日通用 JIT 行为零新回退面）。键 STR ⇒ 数组
 * 必 hash ⇒ guard_ht 无 layout 赌。kt_out 复用为键槽（onekey 时；与 kv
 * 键 TMP 语义互斥——nfetch 1 vs 2）。 */
static int spec_scan(TFunc *fn, uint32_t hdr, uint32_t *acc_out,
                     uint32_t *idx, uint32_t *tmp,
                     uint32_t *base, uint32_t *be_out,
                     uint32_t *kt_out, uint32_t *kb_out, int *ht_out,
                     int *chain_out, int *kstr_out) {
    const char *why = NULL;
    uint32_t badpc = hdr;
    uint32_t cand[JIT_PRE_MAX];
    int nc = preguard_scan(fn, hdr, cand, JIT_PRE_MAX);
    if (nc <= 0) return 0;
    uint32_t i = hdr;
    while (i < fn->n_ops && fn->ops[i].op != OP_JMPZ && fn->ops[i].op != OP_JMPNZ)
        i++;
    if (i >= fn->n_ops) { why = "no cond jmp"; goto out; }
    uint32_t jz = i;                       /* cond 终点（其后为体） */
    uint32_t be = 0;
    for (uint32_t k = hdr + 1; k < fn->n_ops; k++)
        if (fn->ops[k].op == OP_JMP && fn->ops[k].ar == hdr) { be = k; break; }
    if (!be || be <= jz) { why = "no backedge/be<=jz"; goto out; }
    uint32_t fetch_pc = 0, add_pc = 0, inc_pc = 0;
    uint32_t acc = 0, ix = 0, tp = 0, bs = 0;
    uint32_t f1_pc = 0, ktp = 0, kb = 0;   /* 批4a：F1 身份（键槽 base/dst） */
    int nfetch = 0, nadd = 0, ninc = 0;
    /* 批4b-ii 链式模式状态：chain=已进入；cph 相位 1=期望链加 A_c、
     * 2=期望 FETCH 或 RMW 收尾、3=RMW 已收；c_prev=最近链加 dst（首链加
     * 前 0 ⇒ A1 期望 src = ktp=F1 dst）；c_keys=CV 键集（写点约束）；
     * fk_const=任一 fetch 键为 CONST（非链式收尾拒 = 保今日行为）。 */
    int chain = 0, cph = 0, c_rmw = 0;
    uint32_t c_prev = 0;
    uint32_t c_keys[16];
    int nck = 0, fk_const = 0;
    /* 批4b-ii const 链：字面量键编译为 OP_CONST→TMP 后入 FETCH 键位
     * （cconst 集校验 TMP 真是字面量物化）；nconst 计数留待收尾——非链式
     * 一律拒（原 parse 期 default 拒，净效果同；4c 单 fetch const 不随
     * 本刀放行）。cconst_lit 平行记 OP_CONST 的字面量（4c STR lit 闸用）。 */
    uint32_t cconst[24];
    int cconst_lit[24];
    int ncst = 0, nconst = 0;
    /* 批4c：onekey 分流（INC 目标≠键槽的单 fetch）+ F1 键身份/类型 */
    int onekey = 0, kstr = 0;
    uint32_t f1k = 0;
    uint8_t f1k_kind = 0;
    for (uint32_t k = jz + 1; k < be; k++) {
        TOp *op = &fn->ops[k];
        switch (op->op) {
        case OP_CONST:
            nconst++;
            if (op->tr == OPD_TMP && ncst < 24) {
                cconst_lit[ncst] = (op->t1 == OPD_CONST) ? (int)op->a1 : -1;
                cconst[ncst++] = op->ar;
            }
            break;   /* 物化直线写 TMP——结构相位不变，成立性收尾裁决 */
        case OP_FETCH_DIM_R:
            if (chain) {
                /* 链式后续 fetch：相位 2 ⇒ 期望 FETCH；单 base CV、
                 * 键 CV/CONST/字面量物化 TMP、dst TMP */
                if (cph != 2) { why = "chain fetch pos"; badpc = k; goto out; }
                if (op->t1 != OPD_CV || op->a1 != bs) { why = "chain base"; badpc = k; goto out; }
                if (op->tr != OPD_TMP) { why = "chain dst"; badpc = k; goto out; }
                if (op->t2 == OPD_CONST) fk_const = 1;
                else if (op->t2 == OPD_CV) {
                    int dup = 0;
                    for (int ci = 0; ci < nck; ci++) if (c_keys[ci] == op->a2) dup = 1;
                    if (!dup && nck < 16) c_keys[nck++] = op->a2;
                } else if (op->t2 == OPD_TMP) {
                    int isc = 0;
                    for (int ci = 0; ci < ncst; ci++) if (cconst[ci] == op->a2) isc = 1;
                    if (!isc) { why = "chain key tmp"; badpc = k; goto out; }
                } else { why = "chain key"; badpc = k; goto out; }
                tp = op->ar;
                nfetch++;
                fetch_pc = k;
                cph = 1;
                break;
            }
            if (nfetch == 0) {
                /* F1（kv/1-fetch/链式共用起点）——CONST 键推迟到收尾拒
                 * （链式才放行；今日非链式行为净效果不变） */
                if (op->t2 == OPD_CONST) fk_const = 1;
                f1_pc = k; kb = op->a1; ktp = op->ar; ix = op->a2;
                bs = op->a1; tp = op->ar;   /* 1-fetch 形态的 base/dst；
                                             * 2-fetch 形态由 F2 分支覆盖 */
                f1k = op->a2; f1k_kind = op->t2;   /* 批4c：F1 键身份（onekey 收尾判型） */
                nfetch = 1;
            } else if (nfetch == 1) {
                if (op->t2 == OPD_TMP && op->a2 == ktp) {
                    /* 批4a：F2 = 值查表，键恰为 F1 dst 且严格相邻——相邻是
                     * spec_tmp 时序前提（F1 尾置的键 dst 证明被 F2 紧随覆盖，
                     * 中间不得有 op 消费 spec_bit_int 把 STR 键当 INT 证） */
                    if (k != f1_pc + 1) { why = "fetch not adjacent"; badpc = k; goto out; }
                    if (op->t1 != OPD_CV) { why = "fetch2 base not CV"; badpc = k; goto out; }
                    if (op->t2 != OPD_TMP || op->a2 != ktp) { why = "fetch2 key != f1 dst"; badpc = k; goto out; }
                    bs = op->a1; tp = op->ar;
                    nfetch = 2;
                } else if (op->t2 == OPD_CV || op->t2 == OPD_CONST ||
                           op->t2 == OPD_TMP) {
                    /* 批4b-ii 链式入口：F2 键为 CV/CONST/字面量物化 TMP
                     * ⇒ 与 kv 分叉（kv F2 键恒 TMP=F1 dst，上方分支先吃）；
                     * 开关关即拒 = 批4b-i 行为 */
                    if (!chain_fwd_enabled()) { why = "chain fwd off"; badpc = k; goto out; }
                    /* 相邻前提（4a spec_tmp 时序）泛化：F1..F2 间只允许
                     * OP_CONST 字面量物化（直线写自有 TMP，不消费
                     * spec_bit_int；链式 F1 是值 fetch 恒 INT 证、无 STR
                     * 键假证面）——其它 op 一律维持原相邻拒 */
                    {
                        int gap_ok = 1;
                        for (uint32_t q = f1_pc + 1; q < k; q++)
                            if (fn->ops[q].op != OP_CONST) gap_ok = 0;
                        if (!gap_ok) { why = "chain fetch2 not adjacent"; badpc = k; goto out; }
                    }
                    if (op->t2 == OPD_TMP) {
                        int isc = 0;
                        for (int ci = 0; ci < ncst; ci++) if (cconst[ci] == op->a2) isc = 1;
                        if (!isc) { why = "fetch2 key not const tmp"; badpc = k; goto out; }
                    }
                    TOp *f1 = &fn->ops[f1_pc];
                    if (f1->t2 == OPD_TMP) {
                        int isc = 0;
                        for (int ci = 0; ci < ncst; ci++) if (cconst[ci] == f1->a2) isc = 1;
                        if (!isc) { why = "fetch1 key not const tmp"; badpc = k; goto out; }
                    }
                    if (f1->t1 != OPD_CV || op->t1 != OPD_CV || op->a1 != f1->a1) {
                        why = "chain base not uni CV"; badpc = k; goto out;
                    }
                    if (f1->tr != OPD_TMP || op->tr != OPD_TMP) {
                        why = "chain dst not TMP"; badpc = k; goto out;
                    }
                    if (f1->t2 == OPD_CV) c_keys[nck++] = f1->a2;
                    else if (f1->t2 == OPD_CONST) fk_const = 1;
                    if (op->t2 == OPD_CV) c_keys[nck++] = op->a2;
                    else if (op->t2 == OPD_CONST) fk_const = 1;
                    bs = op->a1; tp = op->ar;
                    chain = 1; cph = 1;   /* 期望首链加（A1 src = ktp + tp） */
                    nfetch = 2;
                } else { why = "fetch2 key != f1 dst"; badpc = k; goto out; }
            } else { why = "3rd fetch"; badpc = k; goto out; }
            fetch_pc = k;
            break;
        case OP_ADD: {
            if (chain) {
                if (cph == 1) {
                    /* 期望链加 A_c：dst TMP，src 配对 = {前链 dst｜首链 =
                     * F1 dst(ktp), 本 fetch dst(tp)}（顺序容忍） */
                    if (!nfetch) { why = "chain add pos"; badpc = k; goto out; }
                    if (op->tr != OPD_TMP) { why = "chain add dst not TMP"; badpc = k; goto out; }
                    uint32_t e1 = c_prev ? c_prev : ktp, e2 = tp;
                    int m1 = (op->t1 == OPD_TMP && op->a1 == e1 &&
                              op->t2 == OPD_TMP && op->a2 == e2);
                    int m2 = (op->t1 == OPD_TMP && op->a1 == e2 &&
                              op->t2 == OPD_TMP && op->a2 == e1);
                    if (!m1 && !m2) { why = "chain add src pair"; badpc = k; goto out; }
                    c_prev = op->ar;
                    nadd++; add_pc = k;
                    cph = 2;
                } else if (cph == 2) {
                    /* RMW 收尾：dst CV = acc（单目恒等），另一 src = 末链 dst */
                    if (op->tr != OPD_CV) { why = "chain rmw dst not CV"; badpc = k; goto out; }
                    if (op->t1 == OPD_CV && op->a1 == op->ar &&
                        op->t2 == OPD_TMP && op->a2 == c_prev) {
                        acc = op->ar;
                    } else if (op->t2 == OPD_CV && op->a2 == op->ar &&
                               op->t1 == OPD_TMP && op->a1 == c_prev) {
                        acc = op->ar;
                    } else { why = "chain rmw not paired"; badpc = k; goto out; }
                    nadd++; add_pc = k; c_rmw = 1;
                    cph = 3;
                } else { why = "chain add pos"; badpc = k; goto out; }
                break;
            }
            if (++nadd > 1 || !nfetch) { why = "add pos"; badpc = k; goto out; }
            TOp *f = &fn->ops[fetch_pc];
            /* RMW 配对（镜像 fold_acc_scan）：dst 是 CV 且 dst==src 之一，
             * 另一 src 恰为 FETCH dst（kind 对 kind、idx 对 idx） */
            if (op->tr != OPD_CV) { why = "add dst not CV"; badpc = k; goto out; }
            if (op->t1 == OPD_CV && op->a1 == op->ar &&
                op->t2 == f->tr && op->a2 == f->ar) {
                acc = op->ar;
            } else if (op->t2 == OPD_CV && op->a2 == op->ar &&
                       op->t1 == f->tr && op->a1 == f->ar) {
                acc = op->ar;
            } else { why = "add not rmw-paired"; badpc = k; goto out; }
            add_pc = k;
            break;
        }
        case OP_INC: case OP_DEC:
            if (++ninc > 1 || !nfetch) { why = "inc pos"; badpc = k; goto out; }
            /* 批4a：对 F1 的 key 槽（ix 由 F1 捕获；1-fetch 形态与原
             * fetch.a2 同值）——2-fetch 形态 F2.a2 是键 TMP，INC 必对
             * 循环变量 CV（$k++）。
             * 批4b-ii 链式：ix = INC 自身槽（键是数据键非循环变量，身份
             * 互斥收尾校验；INC 先于链式入口时沿用 F1 键值 → 收尾
             * identity 拒，保守正确）。 */
            if (chain) {
                ix = op->a1;
            } else if (op->a1 == ix) {
                /* read：键==idx 原样（键槽即循环变量，原判据零变化） */
            } else if (nfetch == 1 && nadd == 1) {
                /* 批4c onekey 分流（§8.6）：键≠idx 的单 fetch——idx=INC
                 * 自身槽，键保持 CV/字面量（STR 证在收尾/guard）；INC 先于
                 * 配对 ADD（nadd==0）或 nfetch!=1 一律维持原拒。 */
                if (!onekey_enabled()) { why = "onekey off"; badpc = k; goto out; }
                onekey = 1;
                ix = op->a1;
            } else { why = "inc idx != key"; badpc = k; goto out; }
            if (op->t2 != OPD_UNUSED) { why = "inc dim form"; badpc = k; goto out; }
            /* res 位不设形态判据：编译器常给语句级 $i++ 分配丢弃 res
             * （tr 非 UNUSED）。res 的安全性由下方写点约束兜住——res==acc
             * /base 会命中 pre_op_writes 被拒；res==idx 即 ar==a1 恒等
             * 写（$i=$i++ suffix 语义不变），恒 INT 保持。 */
            inc_pc = k;
            break;
        default:
            why = tphp_op_name(op->op); badpc = k; goto out;  /* 白名单外形态 */
        }
    }
    /* acc 槽号 0 合法（无参闭包首 CV）→ 用 add_pc 作配对成功标志 */
    if (chain) {
        if (!c_rmw || nfetch < 2 || nfetch > 16 || nadd != nfetch ||
            ninc != 1 || !add_pc) {
            if (jit_dbg())
                fprintf(stderr, "[jit] spec reject hdr=%u: chain count fetch=%d add=%d inc=%d rmw=%d\n",
                        hdr, nfetch, nadd, ninc, c_rmw);
            return 0;
        }
    } else {
        if ((nfetch != 1 && nfetch != 2) || nadd != 1 || ninc != 1 || !add_pc) {
            if (jit_dbg())
                fprintf(stderr, "[jit] spec reject hdr=%u: count fetch=%d add=%d inc=%d acc=%u jz=%u be=%u\n",
                        hdr, nfetch, nadd, ninc, acc, jz, be);
            return 0;
        }
        if (onekey) {
            /* 批4c：键判型——CV 键 kstr=1（guard 合取 tag==STR）；字面量
             * 键（OPD_CONST 直折或 OP_CONST 物化 TMP）仅 STR lit 放行
             * （kstr=2 编译期已证），非 STR lit/非字面量 TMP 拒 = 保今日
             * 通用 JIT 行为零新回退面。 */
            if (f1k_kind == OPD_CV) {
                kstr = 1;
            } else {
                int lit = -1;
                if (f1k_kind == OPD_CONST) lit = (int)f1k;
                else if (f1k_kind == OPD_TMP) {
                    for (int ci = 0; ci < ncst; ci++)
                        if (cconst[ci] == f1k) lit = cconst_lit[ci];
                }
                if (lit < 0 || lit >= (int)fn->n_lits || fn->lits[lit].t != T_STR) {
                    why = "onekey lit not str"; badpc = f1_pc; goto out;
                }
                kstr = 2;
            }
        } else if (fk_const || nconst) {
            /* 非链式非 onekey 维持今日 CONST 键拒（原 parse 期拒改收尾拒，
             * 净效果同：kv 带 const / read 体内 const 一并拒） */
            why = "const key"; badpc = f1_pc; goto out;
        }
        if (fetch_pc + 1 != add_pc) { why = "fetch/add not adjacent"; badpc = add_pc; goto out; }
    }
    {
        /* 身份两两互斥（批4a 扩键 TMP/键 base 至六元；1-fetch 与原四元
         * 同式——F1 的 key 槽 ix 即原 fetch.a2）。
         * 批4b-ii 链式：CV 集 {acc,ix,base} + 键 CV（彼此可重复、不得撞
         * 前三；tmp_new 单调 + relocate ⇒ TMP 恒异/与 CV 不交，tp/ktp
         * 入列作双保险） */
        uint32_t id[4 + 16 + 4]; int n = 0;
        if (chain) {
            id[n++] = acc; id[n++] = ix; id[n++] = bs;
            for (int ci = 0; ci < nck; ci++) {
                int bad = (c_keys[ci] == acc || c_keys[ci] == ix || c_keys[ci] == bs);
                for (int a2i = 0; a2i < n; a2i++)
                    if (id[a2i] == c_keys[ci]) bad = 1;
                if (bad) { why = "identity alias"; goto out; }
                id[n++] = c_keys[ci];
            }
            id[n++] = tp; id[n++] = ktp;
        } else {
            id[n++] = acc; id[n++] = ix; id[n++] = bs; id[n++] = tp;
            if (nfetch == 2) { id[n++] = ktp; id[n++] = kb; }
            /* 批4c：onekey 的键槽入列（键≠ix 已由分流保证；CV 键撞
             * acc/ix/bs/tp 拒——TMP 字面量键域不交双保险） */
            if (onekey && f1k_kind == OPD_CV) id[n++] = f1k;
        }
        for (int a = 0; a < n; a++)
            for (int b2 = a + 1; b2 < n; b2++)
                if (id[a] == id[b2]) { why = "identity alias"; goto out; }
    }
    {
        int in = 0, in2 = (nfetch != 2);   /* 1-fetch 无键 base，免查 */
        for (int c = 0; c < nc; c++) {
            if (cand[c] == bs) in = 1;
            if (!chain && nfetch == 2 && cand[c] == kb) in2 = 1;
        }
        if (!in) { why = "base not cand"; goto out; }
        if (!chain && !in2) { why = "key_base not cand"; goto out; }
    }
    for (uint32_t k = jz + 1; k < be; k++) { /* 写点约束 */
        TOp *op = &fn->ops[k];
        if (k != add_pc && pre_op_writes(op, acc)) { why = "body writes acc"; badpc = k; goto out; }
        if (k != inc_pc && pre_op_writes(op, ix)) { why = "body writes idx"; badpc = k; goto out; }
        if (pre_op_writes(op, bs)) { why = "body writes base"; badpc = k; goto out; }
        if (chain) {
            for (int ci = 0; ci < nck; ci++)
                if (pre_op_writes(op, c_keys[ci])) { why = "body writes key"; badpc = k; goto out; }
        } else if (nfetch == 2 && pre_op_writes(op, kb)) { why = "body writes key_base"; badpc = k; goto out; }
        /* 批4c：onekey CV 键循环期零写（STR 证与键值稳定前提；TMP 字面量
         * 键唯一写者 = 体内 OP_CONST 重写同字面量恒值，无需查） */
        if (onekey && f1k_kind == OPD_CV && pre_op_writes(op, f1k)) {
            why = "body writes key"; badpc = k; goto out;
        }
    }
    *acc_out = acc;
    *idx = ix; *tmp = chain ? 0 : tp; *base = bs; *be_out = be;
    /* kt_out 双语义（互斥）：kv = F1 键 TMP（nfetch==2）、onekey = 键槽
     * （nfetch==1，kstr==1 时 guard 读其 tag；kstr==2 不读） */
    *kt_out = chain ? 0 : ((nfetch == 2) ? ktp : (onekey ? f1k : 0));
    *kb_out = (nfetch == 2 && !chain) ? kb : 0;
    *ht_out = (nfetch == 2) || chain || onekey;
    *chain_out = chain;
    *kstr_out = onekey ? kstr : 0;
    return 1;
out:
    if (why && jit_dbg())
        fprintf(stderr, "[jit] spec reject hdr=%u pc=%u: %s\n", hdr, badpc, why);
    return 0;
}

static Trap trap_end(ir_ref e, uint32_t t) { Trap x = { TR_END, e, t }; return x; }
static Trap trap_ret(void)  { Trap x = { TR_RET, 0, 0 }; return x; }
static Trap trap_fail(void) { Trap x = { TR_FAIL, 0, 0 }; return x; }
static Trap trap_lb(void)   { Trap x = { TR_LOOPBACK, 0, 0 }; return x; }

/* P0-14 批5: if 内 continue——分支回边 END 暂存当前层 pending。
 * 1 = 存入；0 = 挡板触发/无循环层/超上限（调用方 fail 回退解释器）。 */
static int lb_push(JitC *j, ir_ref e) {
    if (j->loop_sp <= 0) return 0;
    int k = j->loop_sp - 1;
    if (j->lb_jmpnz[k]) return 0;               /* ||×continue 挡板（见字段注） */
    /* 同层 pending ≤2：≥3（MERGE_N≥4 输入）在 key 等布局组合下亦有 O1
     * 值错（cont_multisite vs ms6 对照——key 有无改变结局）→ 保守挡 */
    if (j->lb_np[k] >= 2) return 0;
    j->lb_pend[k][j->lb_np[k]++] = e;
    return 1;
}

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

/* ---- P0-cond：条件位 truthy 特化（11-04 量化：cond 段≈56% 热路径） ---- */

static int is_jump_target(TFunc *fn, uint32_t pc) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if ((op->op == OP_JMP || op->op == OP_JMPZ || op->op == OP_JMPNZ) &&
            op->ar == pc)
            return 1;
    }
    return 0;
}

/* 批4b-ii：def 处探测其 dst 的链式消费端 pc——（跳过 OP_CONST 字面量物化，
 * 其为直线写 TMP 非块边界）首个引用本 dst 的 ADD ⇒ 消费端；其前恰隔一个
 * FETCH（F1/链 dst 跨一 fetch 型）或不隔（F2/F_i/末链→RMW 型）。stop/jump
 * 门覆盖 (def, 消费端] 全程（镜像刀16 fwd_pair/kfwd）——任一环不符返 0 =
 * 不挂 pay（条目照挂只作 INT 证明，消费端退化槽 load，正确性无虞）。 */
static uint32_t chain_consumer_pc(TFunc *fn, uint32_t def, uint8_t kind,
                                  uint32_t idx, const uint32_t *stops, int nstops) {
    uint32_t p1 = def + 1;
    while (p1 < fn->n_ops && fn->ops[p1].op == OP_CONST) p1++;
    if (p1 >= fn->n_ops) return 0;
    TOp *n1 = &fn->ops[p1];
    int hit1 = n1->op == OP_ADD &&
               ((n1->t1 == kind && n1->a1 == idx) || (n1->t2 == kind && n1->a2 == idx));
    uint32_t cons = 0;
    if (hit1) {
        cons = p1;
    } else if (n1->op == OP_FETCH_DIM_R) {
        uint32_t p2 = p1 + 1;
        while (p2 < fn->n_ops && fn->ops[p2].op == OP_CONST) p2++;
        if (p2 < fn->n_ops) {
            TOp *n2 = &fn->ops[p2];
            if (n2->op == OP_ADD &&
                ((n2->t1 == kind && n2->a1 == idx) || (n2->t2 == kind && n2->a2 == idx)))
                cons = p2;
        }
    }
    if (!cons) return 0;
    for (uint32_t q = def + 1; q <= cons; q++)
        if (stop_has(stops, nstops, q) || is_jump_target(fn, q)) return 0;
    return cons;
}

/* op 对槽 slot 的写点与产出类型三态：1=写（is_bool 出参）、0=不写、
 * -1=不认识（可能写了 → 调用方铁律放弃）。仅列已审计写点的 op：
 * 价值 op 恒写 ar（lower_un/lower_binop/lower_cmp/dim 系列实证），跳转族
 * ar=目标非槽写，INC/DEC RMW 恒写 a1。 */
static int op_slot_write_bool(const TOp *op, const TFunc *fn, uint32_t slot,
                              int *is_bool) {
    *is_bool = 0;
    switch (op->op) {
    case OP_JMP: case OP_JMPZ: case OP_JMPNZ:
        return 0;
    case OP_INC: case OP_DEC:
        return (op->a1 == slot || op->ar == slot) ? 1 : 0;
    /* 恒产 bool（dst=ar） */
    case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE: case OP_LT: case OP_LE:
    case OP_BOOL_VAL: case OP_BOOL_NOT:
    case OP_ISSET_DIM: case OP_ISSET_CV: case OP_ISSET_GLOBAL:
    case OP_ISSET_PROP: case OP_ISSET_OSPROP:
        if (op->ar == slot) { *is_bool = 1; return 1; }
        return 0;
    case OP_CAST:
        /* ct 编码在 lits[a2]：1=int 2=bool 3=float（lower_un 同读） */
        if (op->ar == slot) { *is_bool = fn->lits[op->a2].u.i == 2; return 1; }
        return 0;
    /* 已知写者、非 bool（dst=ar） */
    case OP_CONST: case OP_COPY: case OP_ASSIGN:
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW:
    case OP_CONCAT: case OP_BW_AND: case OP_BW_OR: case OP_BW_XOR:
    case OP_SHL: case OP_SHR: case OP_CMP:
    case OP_NEG: case OP_POS: case OP_BIT_NOT:
    case OP_FETCH_DIM_R: case OP_FETCH_GLOBAL:
    case OP_FETCH_PROP: case OP_FETCH_OSPROP: case OP_FETCH_CCONST:
    case OP_FETCH_CONST:
        return op->ar == slot ? 1 : 0;
    default:
        return -1;
    }
}

/* cond 槽最近写者恒产 bool？——反向直线扫描。判定次序（11-04 修正：
 * 旧版先查跳入 → 循环头=回边目标恰好是 def 点（LT 在头）被误杀，特化
 * 全军覆没）：
 *   ① 首个写者命中 → 返回——其上 (k, jpc] 段已逐点验证无旁入口/无断流，
 *      其余入口只能经 k 本身（必经 def）⇒ 声明 sound；
 *   ② 非写者的跳入目标 → 放弃（下方 def 会被旁路）；JMP 断流 → 放弃；
 *   ③ 不认识的 op → 放弃（可能写了槽）；扫到函数头无写者 → 放弃。
 * jpc 自身是跳入目标 → 直接放弃（存在绕过全部下方 def 的入边）。 */
static int cond_def_is_bool(TFunc *fn, uint32_t jpc, uint8_t kind, uint32_t a) {
    if (kind != OPD_CV && kind != OPD_TMP) return 0;
    if (jpc == 0 || is_jump_target(fn, jpc)) return 0;
    for (uint32_t k = jpc; k > 0; ) {
        k--;
        TOp *op = &fn->ops[k];
        int isb = 0;
        int w = op_slot_write_bool(op, fn, a, &isb);
        if (w < 0) return 0;
        if (w) return isb;
        if (op->op == OP_JMP) return 0;
        if (is_jump_target(fn, k)) return 0;
    }
    return 0;
}

/* 条件位 truthy 统一出口：写者恒 bool → (tag==TRUE) 单节点（跳过全类型链；
 * UNDEF≠TRUE 天然 falsy，与 op_jmpz 的 UNDEF 门一致）；否则全类型链 +
 * UNDEF 门（对齐 op_jmpz/jmpnz：UNDEF=falsy——make_truthy 本体不收
 * UNDEF，保 BOOL_NOT/BOOL_VAL 位与 op_bool_not 默认语义各自一致）。 */
static ir_ref cond_truthy(JitC *j, uint32_t pc, TOp *op) {
    JIT_CTX();
    ir_ref ta, pa, ad;
    ad = cond_addr(j, op->t1, op->a1, &ta, &pa);
    if (cond_def_is_bool(j->fn, pc, op->t1, op->a1))
        return ir_EQ(ta, ir_CONST_I32(JT_TRUE));
    ir_ref t = make_truthy(j, ta, pa, ad);
    return ir_AND(IR_BOOL, t, ir_NOT_B(ir_EQ(ta, ir_CONST_I32((int32_t)T_UNDEF))));
}

/* ASSIGN_DIM 慢路（原 emit 序，快线两分支共用各发一份——dimic miss 多份
 * 发射同款惯例）：CALL helper + raise→deopt RETURN；IF_FALSE 留开由调用
 * 方 END/MERGE。 */
static void emit_assign_dim_slow(JitC *j, ir_ref base, ir_ref key, ir_ref val) {
    JIT_CTX();
    ir_ref t[4] = { j->r_vm, base, key, val };
    ir_ref ok = ir_CALL_N(IR_I32,
                          ir_CONST_ADDR((uintptr_t)tphp_jit_assign_dim),
                          4, t);
    /* raise → 0：IR 立即 deopt（bcall 同款，0008 因子式——
     * IF_TRUE RETURN / IF_FALSE 留开继续，绝不跨 IR 帧 longjmp） */
    ir_ref ifx = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifx);
        ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
    ir_IF_FALSE(ifx);
}

static Trap emit_seq(JitC *j, uint32_t pc, const uint32_t *stops, int nstops) {
    JIT_CTX();
    if (j->fail) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
    for (;;) {
        j->cur_pc = pc;
        if (pc >= j->fn->n_ops) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
        /* 类型特化批1：spec 作用域按 pc 出体自动失效（[spec_lo,spec_hi]
         * 之外的检查点一律恢复完整检查；出体含 cond 段回扫与出口） */
        if (j->spec_lo && (pc < j->spec_lo || pc > j->spec_hi)) {
            j->spec = 0;
            j->spec_lo = j->spec_hi = 0;
            j->spec_tmp_valid = 0;
            j->spec_fwd_valid = 0;
            j->spec_kfwd_valid = 0;
            j->spec_chain = 0;
            j->spec_kstr = 0;
            ctab_clear(j);
            j->spec_base = 0;
            j->spec_ht = 0;
            j->spec_body_tmp = 0;
        }
        /* P0-17③ 批B'：折叠 fast 线的前向汇合（lb_pend 同款机制）——
         * 主线推进到 fwd_pc 时把暂存的 fast 线 END 与主线 END 汇合 */
        if (j->fwd_active && j->fwd_pc == pc) {
            /* 批C slice-1：回边 reconcile——fast 线 ssum 与慢线（generic
             * ADD 已写回槽，此处 load 即权威值）合一；acc_iter 支配回边
             * （守卫保证体无嵌套结构），喂 loop phi 零回边 load。仅当
             * 挂起的 fwd 属于 acc 自己的折叠（fwd_is_acc）才构建——
             * 非 RMW 折叠的 fwd_pay 是别的变量，喂进来即错值 */
            if (j->acc_on && j->acc_phi && j->fwd_is_acc) {
                ir_ref slow_pay = load_pay(j, slot_addr(j, j->acc_slot));
                ir_MERGE_2(j->fwd_end, ir_END());
                j->acc_iter = ir_PHI_2(IR_I64, j->fwd_pay, slow_pay);
            } else {
                ir_MERGE_2(j->fwd_end, ir_END());
            }
            j->fwd_active = 0;
            j->fwd_is_acc = 0;
        }
        if (stop_has(stops, nstops, pc)) return trap_end(ir_END(), pc);

        if (is_backedge_target(j->fn, pc) &&
            !(j->loop_sp > 0 && j->loop_hdr[j->loop_sp - 1] == pc)) {
            if (j->loop_sp >= JIT_MAX_LOOPS) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
            /* 批C slice-1：canonical 守卫 → acc loop phi。init_pay 必须
             * 在 ir_END()（entry 块）前 load；phi 建在 LOOP_BEGIN 后
             * （op1=LOOP_BEGIN 满足 _ir_PHI_SET_OP 断言），回边值在
             * LOOP_END 后补（ir_check 对 loop phi 豁免前向引用） */
            uint32_t acc = (fold_add_enabled() && fold_acc_enabled())
                           ? fold_acc_scan(j->fn, pc) : 0;
            /* POC preguard：先发不变量外提菱形（循环前驱块），保证 fold
             * 的 init_pay/ein 仍落最终汇合块 → LOOP_BEGIN 单入不变 */
            j->pre_n[j->loop_sp] = 0;
            if (preguard_enabled()) {
                uint32_t cand[JIT_PRE_MAX];
                int nc = preguard_scan(j->fn, pc, cand, JIT_PRE_MAX);
                for (int ci = 0; ci < nc; ci++) {
                    ir_ref sa = slot_addr(j, cand[ci]);
                    ir_ref tag = load_tag(j, sa);
                    ir_ref inv = ir_EQ(tag, ir_CONST_I32(T_ARR));
                    ir_ref pay = load_pay(j, sa);
                    ir_ref ifv = ir_IF(inv);
                    ir_IF_TRUE(ifv);
                        /* 非数组槽 payload 不可解引用 → 字段装载只在
                         * inv 真分支；假分支 phi 喂 0 恒不被消费（体内
                         * FETCH 先测 inv 同一枚举） */
                        ir_ref pk = ir_LOAD_U8(ir_ADD_A(pay,
                            ir_CONST_ADDR(offsetof(TArr, packed))));
                        ir_ref n32 = ir_LOAD_U32(ir_ADD_A(pay,
                            ir_CONST_ADDR(offsetof(TArr, n))));
                        ir_ref n64 = ir_ZEXT(IR_U64, n32);
                        ir_ref dt = ir_LOAD_A(ir_ADD_A(pay,
                            ir_CONST_ADDR(offsetof(TArr, data))));
                        ir_ref eT = ir_END();
                    ir_IF_FALSE(ifv);
                        ir_ref eF = ir_END();
                    ir_MERGE_2(eT, eF);
                    j->pre_slot[j->loop_sp][ci] = cand[ci];
                    j->pre_inv[j->loop_sp][ci] = inv;
                    j->pre_a[j->loop_sp][ci] =
                        ir_PHI_2(IR_I64, pay, ir_CONST_I64(0));
                    j->pre_pk[j->loop_sp][ci] =
                        ir_PHI_2(IR_U8, pk, ir_CONST_U8(0));
                    j->pre_n64[j->loop_sp][ci] =
                        ir_PHI_2(IR_U64, n64, ir_CONST_U64(0));
                    j->pre_dt[j->loop_sp][ci] =
                        ir_PHI_2(IR_ADDR, dt, ir_CONST_ADDR(0));
                }
                j->pre_n[j->loop_sp] = nc;
                if (nc > 0 && jit_dbg())
                    fprintf(stderr, "[jit] preguard hdr=%u n=%d\n", pc, nc);
            }
            ir_ref init_pay = 0;
            if (acc) init_pay = load_pay(j, slot_addr(j, acc));
            /* 类型特化批1：preheader guard（OP_SEND_CAP 形态——失败真块短
             * ir_RETURN(OSR_RET_PC)=解释器接管既有机制，FALSE 线继续
             * emit 循环）。guard 三段：g_base helper（ARR+packed+全元素
             * INT 一体查）、acc/idx 槽 tag==INT。通过 → spec 上下文激活，
             * 作用域 [pc,be]。
             * 批2（§5）：OSR chunk 入口同样特化——guard emit 在 chunk
             * entry 块（preheader 菱形之后、LOOP_BEGIN 之前），失败 RETURN
             * (backedge_cur)=解释器执行回边 JMP 续跑（entry 前零副作用，
             * preguard 菱形纯装载、guard helper 纯读），语义与函数级
             * preheader 一致；chunk 尚未执行任何体代码 → resume 干净。 */
            if (type_spec_enabled()) {
                uint32_t s_idx = 0, s_tmp = 0, s_base = 0, s_be = 0;
                uint32_t s_acc = 0, s_ktmp = 0, s_kbase = 0;
                int s_ht = 0, s_chain = 0, s_kstr = 0;
                int s_ok = spec_scan(j->fn, pc, &s_acc, &s_idx, &s_tmp,
                                     &s_base, &s_be, &s_ktmp, &s_kbase, &s_ht,
                                     &s_chain, &s_kstr);
                if (jit_dbg())
                    fprintf(stderr, "[jit] spec scan hdr=%u -> ok=%d acc=%u ht=%d chain=%d kstr=%d\n",
                            pc, s_ok, s_acc, s_ht, s_chain, s_kstr);
                if (s_ok) {
                    ir_ref gargs[1] = { slot_addr(j, s_base) };
                    /* 批4a：form 选 helper——ht（2-fetch/链式/onekey，hash
                     * 键基）走 tphp_jit_spec_guard_ht；packed 形态原 helper 不变 */
                    ir_ref gchk = ir_CALL_N(IR_I32,
                        ir_CONST_ADDR((uintptr_t)(s_ht ? tphp_jit_spec_guard_ht
                                                       : tphp_jit_spec_guard)),
                        1, gargs);
                    ir_ref gok = ir_EQ(gchk, ir_CONST_I32(1));
                    ir_ref gts = load_tag(j, slot_addr(j, s_acc));
                    ir_ref gti = load_tag(j, slot_addr(j, s_idx));
                    ir_ref g = ir_AND(IR_BOOL, ir_AND(IR_BOOL, gok,
                        ir_EQ(gts, ir_CONST_I32(JT_INT))),
                        ir_EQ(gti, ir_CONST_I32(JT_INT)));
                    /* 批4c：kstr==1（CV 键）合取键槽 tag==STR——键循环期
                     * 不由写点约束证 ⇒ 一次 preheader 查全循环有效；键非
                     * STR ⇒ 交解释器（preheader 零副作用重启，批1 协议）。
                     * kstr==2（STR lit）编译期已证不发。 */
                    if (s_kstr == 1) {
                        ir_ref kts = load_tag(j, slot_addr(j, s_ktmp));
                        g = ir_AND(IR_BOOL, g,
                                   ir_EQ(kts, ir_CONST_I32(T_STR)));
                    }
                    ir_ref ifbad = ir_IF(ir_EQ(g, ir_CONST_I32(0)));
                    ir_IF_TRUE(ifbad);
                        ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
                    ir_IF_FALSE(ifbad);
                    /* FALSE 线 = 活线（无 MERGE，镜像 OP_SEND_CAP） */
                    /* 刀15：preheader 消灭体内 FETCH dst 的槽遗留——恒发
                     * gate（旧值任意形态正确释放，每调用一次摊销）+
                     * 置 UNDEF，作为循环内 thin take 免 release 的首迭代
                     * 依据（之后每迭代 dst=take 写入的元素值恒 INT；
                     * 越界 raise 不写 dst → deopt 重跑）。TPHP_TAKE_THIN=0
                     * 不发。批4b-ii 链式不吃消灭（s_chain：值走 ctab 直达
                     * + 物化自带释放门，无 take 消费身份）→ 跳过。 */
                    if (take_thin_enabled() && !s_chain) {
                        ir_ref tsa = slot_addr(j, s_tmp);
                        rel_dst_gate(j, tsa);
                        store_imm(j, tsa, (int32_t)T_UNDEF, ir_CONST_I64(0));
                    }
                    j->spec_acc = s_acc;
                    j->spec_idx = s_idx;
                    j->spec_tmp = s_tmp;
                    j->spec_body_tmp = (take_thin_enabled() && !s_chain) ? s_tmp : 0;
                    j->spec_base = s_base;
                    j->spec_ht = (uint8_t)s_ht;
                    j->spec_chain = (uint8_t)s_chain;
                    j->spec_kstr = (uint8_t)s_kstr;
                    ctab_clear(j);
                    j->spec_lo = pc;
                    j->spec_hi = s_be;
                    j->spec = 1;
                    j->spec_tmp_valid = 0;
                    j->spec_fwd_valid = 0;
                    j->spec_kfwd_valid = 0;
                    if (jit_dbg())
                        fprintf(stderr, "[jit] spec guard hdr=%u acc=%u idx=%u base=%u osr=%d ht=%d chain=%d kstr=%d\n",
                                pc, s_acc, s_idx, s_base, j->osr, s_ht, s_chain, s_kstr);
                } else {
                    /* 本循环不特化 → 清上下文（防上一循环残留；spec 激活
                     * 的体内按白名单不可能出现新循环头，此为纵深保险） */
                    j->spec = 0;
                    j->spec_lo = j->spec_hi = 0;
                    j->spec_tmp_valid = 0;
                    j->spec_fwd_valid = 0;
                    j->spec_kfwd_valid = 0;
                    j->spec_base = 0;
                    j->spec_ht = 0;
                    j->spec_chain = 0;
                    j->spec_kstr = 0;
                    ctab_clear(j);
                    j->spec_body_tmp = 0;
                }
            } else if (jit_dbg()) {
                fprintf(stderr, "[jit] spec skip hdr=%u osr=%d\n", pc, j->osr);
            }
            ir_ref ein = ir_END();
            j->loop_ref[j->loop_sp] = ir_LOOP_BEGIN(ein);
            j->loop_hdr[j->loop_sp] = pc;
            j->loop_cond[j->loop_sp] = 1;
            j->lb_np[j->loop_sp] = 0;          /* 批5: 层复用前清 pending/||标记 */
            j->lb_jmpnz[j->loop_sp] = 0;
            j->acc_on = 0; j->acc_phi = j->acc_iter = 0; j->acc_slot = 0;
            if (acc) {
                j->acc_on = 1;
                j->acc_slot = acc;
                j->acc_phi = ir_PHI_2(IR_I64, init_pay, IR_UNUSED);
            }
            j->loop_sp++;
        }

        TOp *op = &j->fn->ops[pc];
        switch (op->op) {
        case OP_CONST: {
            JIT_CTX();
            TVal v = j->fn->lits[op->a1];
            if (v.t == T_STR) {
                /* 字符串字面量走 helper（释放旧值 + addref 语义）；标量快路 */
                ir_ref t[2] = { slot_addr(j, op->ar),
                                ir_CONST_ADDR((uintptr_t)&j->fn->lits[op->a1]) };
                ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_lit_set), 2, t);
            } else {
                /* 刀2 释放门：字面量恒不与 dst 同槽，无可证免释 → 恒发
                 * （解释器 op_const = tval_addref + wtake，亦先释旧值）；
                 * 刀3 槽级免发除外（dst 槽全写者标量） */
                ir_ref da = slot_addr(j, op->ar);
                if (dst_gate_needed(j, op)) rel_dst_gate(j, da);
                store_imm(j, da, (int32_t)v.t, ir_CONST_I64(v.u.i));
            }
            pc++;
            break;
        }
        case OP_ARRAY_APPEND: {   /* array literal 元素（P0-1）：base=o1, key=o2?, val=(tr,ar) */
            JIT_CTX();
            ir_ref base = slot_addr(j, op->a1);
            ir_ref key = ir_CONST_ADDR(0);   /* NULL → append 形态 */
            if (op->t2 != OPD_UNUSED) {
                if (op->t2 == OPD_CONST) {
                    TVal kv = j->fn->lits[op->a2];
                    ir_ref sc = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
                    store_imm(j, sc, (int32_t)kv.t, ir_CONST_I64(kv.u.i));
                    key = sc;
                } else {
                    key = slot_addr(j, op->a2);
                }
            }
            ir_ref val;
            if (op->tr == OPD_CONST) {
                TVal vv = j->fn->lits[op->ar];
                ir_ref sc = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
                store_imm(j, sc, (int32_t)vv.t, ir_CONST_I64(vv.u.i));
                val = sc;
            } else {
                val = slot_addr(j, op->ar);
            }
            ir_ref t[4] = { j->r_vm, base, key, val };
            ir_ref ok = ir_CALL_N(IR_I32,
                                  ir_CONST_ADDR((uintptr_t)tphp_jit_array_append),
                                  4, t);
            /* raise → 0：同 assign_dim，IF_TRUE RETURN / IF_FALSE 留开 */
            ir_ref ifx = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
            ir_IF_TRUE(ifx);
                ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
            ir_IF_FALSE(ifx);
            pc++;
            break;
        }
        case OP_ARRAY_INIT: {
            JIT_CTX();
            ir_ref t[2] = { j->r_vm, slot_addr(j, op->ar) };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_array_init), 2, t);
            pc++;
            break;
        }
        case OP_ASSIGN_DIM: {   /* append（key==NULL）+ 带键（P0-10）：base=o1, key=o2?, val=(tr,ar) */
            JIT_CTX();
            ir_ref base = slot_addr(j, op->a1);
            ir_ref key = ir_CONST_ADDR(0);   /* NULL → append 形态 */
            if (op->t2 != OPD_UNUSED) {
                if (op->t2 == OPD_CONST) {
                    TVal kv = j->fn->lits[op->a2];
                    ir_ref sc = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
                    store_imm(j, sc, (int32_t)kv.t, ir_CONST_I64(kv.u.i));
                    key = sc;
                } else {
                    key = slot_addr(j, op->a2);
                }
            }
            ir_ref val;
            if (op->tr == OPD_CONST) {
                TVal vv = j->fn->lits[op->ar];
                ir_ref sc = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
                store_imm(j, sc, (int32_t)vv.t, ir_CONST_I64(vv.u.i));
                val = sc;
            } else {
                val = slot_addr(j, op->ar);
            }
            /* P0-append 快线（11-04 新地图 array-push≈3.5×=杠杆第一）：
             * 仅 append 形态（t2==UNUSED）。快线条件 AND——base 是数组
             * （排除 UNDEF auto-viv/T_STR Error/TypeError）、rc<=1（tarr_cow
             * 原地免复制）、packed、dused<dcap（免 grow 分配）、val 非 rc
             * （tval_addref no-op）→ 镜像 tarr_append packed 分支纯内存写
             * （data[dused]=val pay先tag后 + dused/n/next_idx/bpending 自增；
             * packed 无洞 ⇒ dused 位恒 UNDEF/fresh，不 release 与 C 逐位同）。
             * 零 CALL/零分配/零 raise/零槽写 → 无需 deopt 门；任一条件
             * 不满足 → 原 tphp_jit_assign_dim 慢路两分支各发一份完整兜底
             * （语义零漂移）。keyed 形态/开关关 → 原直通。 */
            if (append_fast_enabled() && op->t2 == OPD_UNUSED) {
                ir_ref ifa = ir_IF(ir_EQ(load_tag(j, base), ir_CONST_I32(T_ARR)));
                ir_IF_TRUE(ifa);
                    ir_ref ap = load_pay(j, base);
                    ir_ref rc = ir_LOAD_I32(
                        ir_ADD_A(ap, ir_CONST_ADDR(offsetof(GCHead, rc))));
                    ir_ref pk = ir_LOAD_U8(
                        ir_ADD_A(ap, ir_CONST_ADDR(offsetof(TArr, packed))));
                    ir_ref du = ir_LOAD_I32(
                        ir_ADD_A(ap, ir_CONST_ADDR(offsetof(TArr, dused))));
                    ir_ref dc = ir_LOAD_I32(
                        ir_ADD_A(ap, ir_CONST_ADDR(offsetof(TArr, dcap))));
                    ir_ref vt = load_tag(j, val);
                    ir_ref ok = ir_AND(IR_BOOL,
                        ir_AND(IR_BOOL, ir_LE(rc, ir_CONST_I32(1)),
                               ir_EQ(pk, ir_CONST_U8(1))),
                        ir_AND(IR_BOOL, ir_LT(du, dc),
                               ir_LT(vt, ir_CONST_I32(T_STR))));
                    ir_ref ifok = ir_IF(ok);
                    ir_IF_TRUE(ifok);
                        ir_ref data = ir_LOAD_A(
                            ir_ADD_A(ap, ir_CONST_ADDR(offsetof(TArr, data))));
                        ir_ref leaf = ir_ADD_A(data,
                            ir_SHL_U64(ir_ZEXT(IR_U64, du), ir_CONST_U64(4)));
                        ir_STORE(leaf, load_pay(j, val));
                        ir_STORE(ir_ADD_A(leaf, ir_CONST_ADDR(TVAL_TAG)), vt);
                        ir_STORE(ir_ADD_A(ap,
                                    ir_CONST_ADDR(offsetof(TArr, dused))),
                                 ir_ADD_I32(du, ir_CONST_I32(1)));
                        ir_ref an = ir_ADD_A(ap,
                            ir_CONST_ADDR(offsetof(TArr, n)));
                        ir_STORE(an, ir_ADD_I32(ir_LOAD_I32(an),
                                                ir_CONST_I32(1)));
                        ir_ref ani = ir_ADD_A(ap,
                            ir_CONST_ADDR(offsetof(TArr, next_idx)));
                        ir_STORE(ani, ir_ADD_I64(ir_LOAD_I64(ani),
                                                 ir_CONST_I64(1)));
                        ir_ref abp = ir_ADD_A(ap,
                            ir_CONST_ADDR(offsetof(TArr, bpending)));
                        ir_STORE(abp, ir_ADD_I32(ir_LOAD_I32(abp),
                                                 ir_CONST_I32(1)));
                        ir_ref eF = ir_END();
                    ir_IF_FALSE(ifok);
                        emit_assign_dim_slow(j, base, key, val);
                        ir_ref eSK = ir_END();
                    ir_MERGE_2(eF, eSK);
                    ir_ref eArr = ir_END();
                ir_IF_FALSE(ifa);
                    emit_assign_dim_slow(j, base, key, val);
                    ir_ref eNo = ir_END();
                ir_MERGE_2(eArr, eNo);
                pc++;
                break;
            } else if (setdim_fast_enabled() && op->t2 != OPD_UNUSED) {
                /* 刀4（11-05 keyed 覆盖写快路，目标 = CI linux-x64 rmw 4.0×）：
                 * $a[$i]=$a[$i]+1 形态此前每迭代整链 tphp_jit_assign_dim
                 * helper CALL（norm_key/cow/分发全付）。镜像 tarr_set packed
                 * 原地替换支（array.c:210：packed 无洞 → release+覆盖三行，
                 * n/next_idx/bpending 全不动、无 grow/compact）：
                 * - base==T_ARR（排 UNDEF autoviv / T_STR Error / scalar
                 *   TypeError）&& rc<=1（tarr_cow 原地免复制）&& packed；
                 * - key tag==INT && (u64)key<n——单条 ULT（BITCAST 后负键
                 *   = 巨大无符号恒假）镜像 dimic 同款，等价 C 的
                 *   i>=0 && (uint64)i<n；
                 * - val.tag<T_STR（tval_addref no-op）——与 helper 按值传参
                 *   语义对齐：vp/vt 先快照再 release 旧元素（elem 内对象
                 *   dtor 可跑用户码改 val 槽，快照后不受影响）；
                 * - rel_dst_gate 释放旧元素（tag∈[T_STR..T_CLOSURE] 才
                 *   CALL，UNDEF 洞/标量空转）→ pay/tag 覆盖写。
                 * 任一条件不满足 → 慢路两分支各发完整兜底（norm/COW/grow/
                 * compact/assoc/非法键 raise 全在其内，语义零漂移）。
                 * append 形态走既有 APPEND 快路；TPHP_SETDIM_FAST=0 关。 */
                ir_ref ifa = ir_IF(ir_EQ(load_tag(j, base),
                                         ir_CONST_I32(T_ARR)));
                ir_IF_TRUE(ifa);
                    ir_ref ap = load_pay(j, base);
                    ir_ref rc = ir_LOAD_I32(
                        ir_ADD_A(ap, ir_CONST_ADDR(offsetof(GCHead, rc))));
                    ir_ref pk = ir_LOAD_U8(
                        ir_ADD_A(ap, ir_CONST_ADDR(offsetof(TArr, packed))));
                    ir_ref n64 = ir_ZEXT(IR_U64, ir_LOAD_U32(
                        ir_ADD_A(ap, ir_CONST_ADDR(offsetof(TArr, n)))));
                    ir_ref kt = load_tag(j, key);
                    ir_ref kp = load_pay(j, key);
                    ir_ref vt = load_tag(j, val);
                    ir_ref ok = ir_AND(IR_BOOL,
                        ir_AND(IR_BOOL, ir_LE(rc, ir_CONST_I32(1)),
                               ir_NE(pk, ir_CONST_U8(0))),
                        ir_AND(IR_BOOL, ir_EQ(kt, ir_CONST_I32(JT_INT)),
                               ir_ULT(ir_BITCAST_U64(kp), n64)));
                    ok = ir_AND(IR_BOOL, ok,
                                ir_LT(vt, ir_CONST_I32(T_STR)));
                    ir_ref ifok = ir_IF(ok);
                    ir_IF_TRUE(ifok);
                        ir_ref vp = load_pay(j, val);   /* 按值快照（C 传参序） */
                        ir_ref data = ir_LOAD_A(
                            ir_ADD_A(ap, ir_CONST_ADDR(offsetof(TArr, data))));
                        ir_ref leaf = ir_ADD_A(data,
                            ir_SHL_U64(ir_BITCAST_U64(kp),
                                       ir_CONST_U64(4)));
                        rel_dst_gate(j, leaf);          /* release 旧元素 */
                        ir_STORE(leaf, vp);
                        ir_STORE(ir_ADD_A(leaf, ir_CONST_ADDR(TVAL_TAG)), vt);
                        ir_ref eF = ir_END();
                    ir_IF_FALSE(ifok);
                        emit_assign_dim_slow(j, base, key, val);
                        ir_ref eSK = ir_END();
                    ir_MERGE_2(eF, eSK);
                    ir_ref eArr = ir_END();
                ir_IF_FALSE(ifa);
                    emit_assign_dim_slow(j, base, key, val);
                    ir_ref eNo = ir_END();
                ir_MERGE_2(eArr, eNo);
                pc++;
                break;
            }
            ir_ref t[4] = { j->r_vm, base, key, val };
            ir_ref ok = ir_CALL_N(IR_I32,
                                  ir_CONST_ADDR((uintptr_t)tphp_jit_assign_dim),
                                  4, t);
            /* raise → 0：IR 立即 deopt（bcall 同款，0008 因子式——
             * IF_TRUE RETURN / IF_FALSE 留开继续，绝不跨 IR 帧 longjmp） */
            ir_ref ifx = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
            ir_IF_TRUE(ifx);
                ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
            ir_IF_FALSE(ifx);
            pc++;
            break;
        }
        case OP_FETCH_DIM_R: {
            JIT_CTX();
            /* P0-17③ 批B'：折叠模式检测——紧邻 ADD 且 src 含本 op 的 dst
             * （$s += $a[$i] 的 RMW 两连 / $x = $a[$i] + $y 的值产），
             * 另一 src 必须是槽（CV/TMP；CONST 形态保守不折叠）。
             * fast：packed 命中 && 双 INT && 无溢出 → 两 op 效果直出
             *      （dst 直写，fwd 前向汇合跳过 ADD）；slow：原 dst 写，
             *      主线 ADD 照常（语义零漂移）。 */
            uint32_t np1 = pc + 1;
            TOp *aop = (np1 < j->fn->n_ops) ? &j->fn->ops[np1] : NULL;
            /* 折叠门（fold_add_enabled，注释见上）+ 单 pending 防护：
             * fwd 已挂起时第二个折叠会覆盖 fwd_end/fwd_pc（第一个 fast
             * 块悬空无汇合 = CFG 破坏），保守拒编 */
            bool fold_add = false;
            uint32_t other_t = 0, other_a = 0;
            if (fold_add_enabled() && !j->fwd_active &&
                aop && aop->op == OP_ADD && aop->tr != OPD_UNUSED) {
                if (aop->t1 == op->tr && aop->a1 == op->ar &&
                    (aop->t2 == OPD_CV || aop->t2 == OPD_TMP)) {
                    fold_add = true;
                    other_t = aop->t2; other_a = aop->a2;
                } else if (aop->t2 == op->tr && aop->a2 == op->ar &&
                           (aop->t1 == OPD_CV || aop->t1 == OPD_TMP)) {
                    fold_add = true;
                    other_t = aop->t1; other_a = aop->a1;
                }
            }
            ir_ref key_addr;
            if (op->t2 == OPD_CONST) {
                TVal kv = j->fn->lits[op->a2];
                key_addr = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
                store_imm(j, key_addr, (int32_t)kv.t, ir_CONST_I64(kv.u.i));
            } else {
                key_addr = slot_addr(j, op->a2);
            }
            /* P0-13：per-site IC（编译时刻懒分配，emit 在编译线程单次
             * 执行；v3 统一初始化——共享快照+cases 哨兵恒非 NULL） */
            if (!op->ic) {
                op->ic = tm_malloc(sizeof(DimIC));
                tphp_dim_ic_init(op->ic);
            }
            /* P0-13 v2：命中路径全内联（packed 直取 / DimIC 链），miss/
             * 非数组/raise 全落 v1 helper+deopt——对外语义与 v1 单 call
             * 完全一致，仅热路径消掉 call 框架与 TVal 拷贝。 */
            DimIcCtx g;
            g.j = j;
            g.dst = slot_addr(j, op->ar);
            g.base = slot_addr(j, op->a1);
            g.key = key_addr;
            g.ic = ir_CONST_ADDR((uintptr_t)op->ic);
            g.a = g.kt = g.kp = IR_UNUSED;
            g.kfwd_prod = g.kfwd_mat = 0;
            g.kfwd_pay = g.kfwd_tag = 0;
            g.kt_fix = 0;
            /* 批4b-i key-fwd 消费（F2 头）：紧邻 F1 挂起的 (pay,tag) phi
             * 按 kind+idx 双等接进键位——免槽 load；键槽恒不写，所有
             * helper/deopt 出口（dimic_miss）先物化（take 序）保证解释器
             * 复跑读到 owned 值。消费即清，F2 尾由 kfwd_on=0 自清残留。 */
            int kfwd_in = j->spec_kfwd_valid && op->t2 == OPD_TMP &&
                          op->a2 == j->spec_kfwd_idx;
            if (kfwd_in) {
                g.kp = j->spec_kfwd_pay;
                g.kt = j->spec_kfwd_tag;
                g.kfwd_mat = 1;
                g.kfwd_pay = j->spec_kfwd_pay;
                g.kfwd_tag = j->spec_kfwd_tag;
                j->spec_kfwd_valid = 0;
            } else {
                g.kp = load_pay(j, g.key);
            }
            /* 批4a：链叶 thin 透传（ht 作用域 + body_tmp 身份恒等；
             * 元素恒 INT 由 guard_ht 证、槽遗留由 preheader 消灭证——
             * 与 spec_c 路径 3783 处的 thin 同一证明面） */
            g.thin = (j->spec && j->spec_ht && j->spec_body_tmp &&
                      op->ar == j->spec_body_tmp) ? 1 : 0;
            int pi = preguard_lookup(j, op->a1);
            int lvl = j->loop_sp - 1;
            /* POC preguard：命中本层候选 → 守卫条件与不变量字段取循环
             * 前驱块外提值（inv/payload/packed/n/data），key 残余（kt/
             * ULT）与慢路 v1 helper 照旧；未命中 → 原逐迭代装载 */
            /* 类型特化批2（PLAN/type-spec.md §5）：spec 作用域内本 fetch
             * 的 base 恰为 guard 证过的槽（spec_base——cond 区异 base 的
             * preguard 候选无证不折）且 key 恒 INT（spec_bit_int=批1 证：
             * guard + 溢出 RETURN 链）→ guard（纯读 helper，与 preguard
             * 菱形同块、无中途写）已证 ARR+packed+全元素 INT ⇒ inv≡真、
             * packed≡真 → ifa 外层、pcond packed 门、非 packed/非数组
             * 回退与 IC 层叠链（bound 外的层叠 cmp）全部不可达 → 不 emit；
             * 只留 [bound→取值 | 越界→miss 兜底] 单菱形（idx<n 不在
             * guard 面，越界照走 v1 helper）。结构消除非降级：miss/raise/
             * deopt 语义与原结构一致。kt 装载随之免发。未折叠 → 原结构
             * 零改动（含批1 key tag 门）。 */
            /* 批4a：键 TMP 的 INT 证在 ht 作用域无效（键=F1 取值=STR，
             * spec_tmp 在 F1 尾置位的窗口是假证——相邻约束保证 F2 紧随
             * 覆盖，此处按 kind 显式排除）。spec_c（批2 packed 结构证）
             * 随之对 ht 排除：guard_ht 已证 !packed，走 bound-only 折叠
             * 必错（hash 无 bound 语义）。 */
            int key_int_ok = spec_bit_int(j, op->t2, op->a2)
                             && !(j->spec_ht && op->t2 == OPD_TMP);
            int spec_c = j->spec && !j->spec_ht && pi >= 0 && op->a1 == j->spec_base
                         && key_int_ok;
            /* 批4a-ii：spec_ht 直探分支（与批2 spec_c 并列互斥）——ht 点
             * 整段绕过 inv/pcond/bound/链，直探+fwd 由 emit_ht_fetch 内发。
             * fwd 形状门（刀16 同款配对/stop/jump 判据）在此层算——
             * is_jump_target 等仅 case 作用域可见；身份条件由 g.thin 承载
             * （spec_ht+body_tmp+恒等）。 */
            int spec_ht_ok = j->spec && j->spec_ht && pi >= 0 && op->a1 == j->spec_base;
            /* 批4c：单 fetch ht 形态键 STR 已证（kstr=1 guard 合取键槽
             * tag==STR / kstr=2 STR lit 编译期）→ 预置 kt=CONST T_STR 免
             * 槽 tag 装载，emit_ht_fetch 据 kt_fix 跳过整个 STR 分派菱形
             * （kv/chain/read 的 spec_kstr 恒 0 ⇒ 零变化）。 */
            if (j->spec_kstr && spec_ht_ok) {
                g.kt = ir_CONST_I32(T_STR);
                g.kt_fix = 1;
            }
            int fwd_pair = !fold_add && fetch_fwd_enabled() && aop &&
                aop->op == OP_ADD && aop->tr == OPD_CV &&
                ((aop->t1 == OPD_CV && aop->a1 == aop->ar &&
                  aop->t2 == op->tr && aop->a2 == op->ar) ||
                 (aop->t2 == OPD_CV && aop->a2 == op->ar &&
                  aop->t1 == op->tr && aop->a1 == op->ar)) &&
                !stop_has(stops, nstops, np1) &&
                !is_jump_target(j->fn, np1);
            int fwd_on = 0;
            /* 批4b-i key-fwd（§8.6）：spec_ht 2-fetch 形态中本 fetch 是
             * F1——紧邻 FETCH 的键恰为本 dst（emit 期复验 spec_scan F2
             * 判据）→ (pay,tag) 经三层 MERGE phi 汇出，紧邻 F2 键位直供，
             * 免 F1 STR take（addref/release/misses/16B store）+ F2 槽
             * 往返。rc 论证：热路径键只被探针身份比较与 hash 读写（非所
             * 有权），不落槽不逃逸；kb 元素由 kb 自持（spec_scan 写点约
             * 束体内零写）→ 免 addref 安全；helper/deopt 出口 dimic_miss
             * 物化（take 序）→ 解释器复跑读 owned 值、帧销毁配平。
             * 块连续判据镜像刀16 fwd_pair（防 phi 跨块/外部跳入失准）。 */
            int kfwd_on = key_fwd_enabled() && !fold_add &&
                j->spec && j->spec_ht && op->tr == OPD_TMP &&
                aop && aop->op == OP_FETCH_DIM_R && aop->t1 == OPD_CV &&
                aop->t2 == OPD_TMP && aop->a2 == op->ar &&
                /* F2 必走 spec_ht 直探（=必消费本 phi）的静态保证：
                 * base==spec_base + 在 preguard cand（镜像 spec_scan 对
                 * bs 的两条检查）——否则 F1 跳过 take 而 F2 读 UNDEF 槽 */
                aop->a1 == j->spec_base &&
                preguard_lookup(j, aop->a1) >= 0 &&
                !stop_has(stops, nstops, np1) &&
                !is_jump_target(j->fn, np1);
            /* 批4b-ii 链式 fwd 门：消费端存在（chain_consumer_pc = np1/np2
             * 引用本 dst 的 ADD 且 def→消费端全程无 stop/jump 断块，形状
             * 由 spec_scan 证、此处只管块连续）+ pay 直达总闸
             * fetch_fwd_enabled（关 = 条目 pay=0 纯证明模式）。门失败仍
             * 挂条目（pay=0/unwritten=0）——INT 证明独立于 pay 存在，
             * 消费端退化槽 load（此时槽必已写：fwd 关 ⇒ take 照写）。 */
            int c_fwd = 0;
            if (j->spec && j->spec_chain && spec_ht_ok && !fold_add &&
                fetch_fwd_enabled() &&
                chain_consumer_pc(j->fn, pc, op->tr, op->ar, stops, nstops))
                c_fwd = 1;
            g.kfwd_prod = kfwd_on ? 1 : 0;
            /* phi 生产三变量（外层声明，尾部挂起用；有效 ref 恒 >0） */
            ir_ref kfwd_hit_pay = 0, kfwd_hit_tag = 0;
            ir_ref kfwd_m0_pay = 0, kfwd_m0_tag = 0;  /* bound 越界线输入 */
            ir_ref kfwd_cf_pay = 0, kfwd_cf_tag = 0;  /* 非数组线输入 */
            ir_ref kfwd_p1 = 0, kfwd_p1t = 0;    /* bound 层 MERGE 后 */
            ir_ref kfwd_p2 = 0, kfwd_p2t = 0;    /* packed/链层 MERGE 后 */
            ir_ref kfwd_p3 = 0, kfwd_p3t = 0;    /* 主尾 MERGE 后（挂起用） */
            ir_ref ifa = 0, ifp = 0;
            if (spec_ht_ok) {
                /* 批4b-ii：链式走 c_fwd（消费端探测门）；kv/1-fetch 维持
                 * 刀16 形状门 + thin（双模式由 scan 分叉互斥） */
                fwd_on = j->spec_chain ? c_fwd : (fwd_pair && g.thin);
                emit_ht_fetch(j, &g, pi, lvl, fwd_on);
            } else {
            if (!spec_c) {
                g.kt = load_tag(j, g.key);
                ifa = ir_IF(pi >= 0
                    ? j->pre_inv[lvl][pi]
                    : ir_EQ(load_tag(j, g.base), ir_CONST_I32(T_ARR)));
                ir_IF_TRUE(ifa);
                g.a = (pi >= 0) ? j->pre_a[lvl][pi] : load_pay(j, g.base);
                /* 互斥键路径：packed int 直取 vs hash 快照。
                 * 类型特化批1：key=idx 恒 INT（guard + 溢出 RETURN 证）
                 * → 免 key tag 检查（$i tag cmp+sete）；批2 折叠在上。 */
                ir_ref pkv = pi >= 0
                    ? j->pre_pk[lvl][pi]
                    : ir_LOAD_U8(ir_ADD_A(g.a,
                          ir_CONST_ADDR(offsetof(TArr, packed))));
                ir_ref pcond = key_int_ok
                    ? ir_NE(pkv, ir_CONST_U8(0))
                    : ir_AND(IR_BOOL, ir_NE(pkv, ir_CONST_U8(0)),
                             ir_EQ(g.kt, ir_CONST_I32(JT_INT)));
                ifp = ir_IF(pcond);
                ir_IF_TRUE(ifp);
            }
                    /* (u64)key < (u64)n 单条无符号比较：天然覆盖 i<0
                     * （负数 = 巨大无符号）与 i>=n，与 helper 的
                     * i>=0 && (uint32)i<n 等价 */
                    ir_ref n64 = pi >= 0
                        ? j->pre_n64[lvl][pi]
                        : ir_ZEXT(IR_U64,
                              ir_LOAD_U32(ir_ADD_A(g.a,
                                  ir_CONST_ADDR(offsetof(TArr, n)))));
                    /* 刀16（FETCH 值直达）：thin 条件 + emit 期证明
                     * [pc+1 恰为配对 ADD（镜像 spec_scan RMW 判据）且
                     * 非 stop 点（否则 ADD 由续块发射，fwd 跨块失准）]
                     * → hit 线只 LOAD 元素 pay 不落 dst 槽，miss 线
                     * load dst（helper 成功写值，spec 下越界恒 raise
                     * 即该线实死，load 仅作 phi 输入保结构正确），
                     * MERGE 处 phi 桥给紧邻 ADD——免 take 2 store +
                     * ADD 1 load 的槽往返。 */
                    if (fwd_pair)   /* 形状门已在外层（spec_ht 分支同源） */
                        fwd_on = spec_c && j->spec_body_tmp &&
                                 op->ar == j->spec_body_tmp;
                    ir_ref fwd_hit = 0, fwd_miss = 0;
                    ir_ref ifb = ir_IF(ir_ULT(ir_BITCAST_U64(g.kp), n64));
                    ir_IF_TRUE(ifb);
                        ir_ref data = pi >= 0
                            ? j->pre_dt[lvl][pi]
                            : ir_LOAD_A(ir_ADD_A(g.a,
                                  ir_CONST_ADDR(offsetof(TArr, data))));
                        ir_ref leaf = ir_ADD_A(data,
                            ir_SHL_U64(ir_BITCAST_U64(g.kp), ir_CONST_U64(4)));
                        if (!fold_add) {
                            /* 刀15 thin：spec_c（元素恒 INT 证）+ 体内
                             * FETCH 身份（preheader 已 gate+UNDEF 消灭
                             * spec_body_tmp 槽）双满足才瘦身；cond 区
                             * FETCH/未消灭槽一律原样三门。刀16：fwd_on
                             * 时连 16B store 一并免（值走 phi）。批4b-i：
                             * kfwd_on 时键槽整段不写——(pay,tag) 双 load
                             * 作 phi 输入直供紧邻 F2（键不逃逸免 rc，见
                             * 门注释）。 */
                            if (kfwd_on) {
                                kfwd_hit_pay = load_pay(j, leaf);
                                kfwd_hit_tag = load_tag(j, leaf);
                            } else if (fwd_on)
                                fwd_hit = ir_LOAD_I64(leaf);
                            else
                                dimic_take(&g, leaf,
                                    spec_c && j->spec_body_tmp &&
                                    op->ar == j->spec_body_tmp);
                        } else {
                            /* fast/slow 双线（P0-17③ 批B'）；批C slice-1：
                             * RMW acc 且守卫激活 → $s pay 读 loop phi
                             * （零槽 load）、写只 store pay（tag 恒 entry
                             * 值不被 fast 线触碰 → slot 恒 current，
                             * deopt/exit 安全） */
                            ir_ref vtag = ir_LOAD_I32(
                                ir_ADD_A(leaf, ir_CONST_ADDR(TVAL_TAG)));
                            ir_ref vpay = ir_LOAD_I64(leaf);
                            /* dst 槽 = ar（tr 是 kind！原 B' 版误用 tr 当
                             * idx——OPD_CV=2 恰与 bench acc 槽 2 重合而
                             * 被掩蔽；acc 在其他槽位会写坏变量） */
                            ir_ref daddr = slot_addr(j, aop->ar);
                            int phion = j->acc_on && j->acc_phi &&
                                        aop->tr == OPD_CV &&
                                        aop->ar == j->acc_slot &&
                                        aop->t1 == OPD_CV && aop->a1 == aop->ar;
                            ir_ref oaddr = (other_t == OPD_CV || other_t == OPD_TMP)
                                ? slot_addr(j, other_a)
                                : daddr;   /* 形态检测已保证槽；兜底指向 dst */
                            ir_ref otag = load_tag(j, oaddr);
                            ir_ref opay = phion ? j->acc_phi : load_pay(j, oaddr);
                            ir_ref okint = ir_AND(IR_BOOL,
                                ir_EQ(vtag, ir_CONST_I32(JT_INT)),
                                ir_EQ(otag, ir_CONST_I32(JT_INT)));
                            /* 投机加法（INT 加法无陷阱）+ 溢出检查归 slow */
                            ir_ref ssum = ir_ADD_I64(opay, vpay);
                            ir_ref nof = ir_LT(
                                ir_AND_I64(ir_XOR_I64(opay, ssum),
                                           ir_XOR_I64(vpay, ssum)),
                                ir_CONST_I64(0));
                            ir_ref okf = ir_AND(IR_BOOL, okint, nof);
                            ir_ref iff = ir_IF(okf);
                            ir_IF_TRUE(iff);
                                /* fast：dst 直写（两 op 效果），fwd 前向汇合
                                 * ——fast 块终止于 fwd_end（脱离本链） */
                                /* 刀2 释放门：other 即 dst 槽（含 phion，
                                 * 恒 other==ar）时 otag==daddr tag，okint
                                 * 已证 dst 为 INT → 免发；否则 dst 旧值
                                 * 无约束，写前释放（解释器 ADD 亦 wtake） */
                                if (other_a != aop->ar &&
                                    dst_gate_needed(j, aop))
                                    rel_dst_gate(j, daddr);
                                if (phion) {
                                    store_pay(j, daddr, ssum);
                                } else {
                                    store_imm(j, daddr, (int32_t)JT_INT, ssum);
                                }
                                j->fwd_end = ir_END();
                                j->fwd_pc = np1 + 1;
                                j->fwd_active = 1;
                                j->fwd_is_acc = (uint8_t)phion;
                                j->fwd_pay = ssum;
                            ir_IF_FALSE(iff);
                                dimic_take(&g, leaf, 0);   /* slow：原 dst 写，
                                    活线延续（不 END，ifb-TRUE 块尾统一封） */
                        }
                        ir_ref eP1 = ir_END();
                    ir_IF_FALSE(ifb);
                        dimic_miss(&g, false);   /* packed 越界非 IC 键形态 */
                        /* 刀16 phi 输入：helper 成功线写 dst 后取 pay。
                         * spec 下越界恒 raise（packed+INT key 证）→ 该线
                         * 实死，load 仅为 phi 结构占位（若可达则值亦真）。 */
                        if (fwd_on)
                            fwd_miss = load_pay(j, g.dst);
                        /* 批4b-i：kfwd miss 线——helper 成功写键槽后取
                         * (pay,tag) 作 phi 输入（raise 线 RETURN 不汇流）。
                         * kfwd_on⟹aop 是 FETCH⟹fwd_on 恒 0，两门互斥。 */
                        if (kfwd_on) {
                            kfwd_m0_pay = load_pay(j, g.dst);
                            kfwd_m0_tag = load_tag(j, g.dst);
                        }
                        ir_ref eP0 = ir_END();
                    ir_MERGE_2(eP1, eP0);
                    if (fwd_on)
                        j->spec_fwd_pay = ir_PHI_2(IR_I64, fwd_hit, fwd_miss);
                    if (kfwd_on) {
                        /* 第一层：bound 命中（leaf 双 load）/越界（helper
                         * 写槽后双 load）汇流——与刀16 fwd phi 同 MERGE 并列 */
                        kfwd_p1 = ir_PHI_2(IR_I64, kfwd_hit_pay, kfwd_m0_pay);
                        kfwd_p1t = ir_PHI_2(IR_I32, kfwd_hit_tag, kfwd_m0_tag);
                    }
            if (!spec_c) {
                    ir_ref ePack = ir_END();
                ir_IF_FALSE(ifp);
                    ir_ref eIc;
                    if (j->spec && j->spec_ht) {
                        /* 批4a：ht 作用域跳过 generic 8-fail 门——形态由
                         * guard+体内零写整循环锁定（快照恒成立），misses
                         * 字段本路径零读者（镜像批2 spec_c 不 emit 论证；
                         * 冷态首几轮 chain 快照失败 → miss → helper 装填
                         * 照常，与门无关） */
                        eIc = dimic_ic_chain(&g, 0);
                    } else {
                    /* generic 回退门（JSC 同款）：单态假设破灭的 site
                     * （轮转键/形态翻转）≥8 次链失败 → 跳链直 call
                     * helper（v1 成本），不再付失败链 */
                    ir_ref mc = ir_LOAD_I32(
                        ir_ADD_A(g.ic, ir_CONST_ADDR(offsetof(DimIC, misses))));
                    ir_ref ifg = ir_IF(ir_LT(mc, ir_CONST_I32(8)));
                    ir_IF_TRUE(ifg);
                        ir_ref eChain = dimic_ic_chain(&g, 0);
                    ir_IF_FALSE(ifg);
                        dimic_miss(&g, false);   /* 锁死态不计数 */
                        ir_ref eLocked = ir_END();
                    ir_MERGE_2(eChain, eLocked);
                    eIc = ir_END();
                    }
                ir_MERGE_2(ePack, eIc);
                if (kfwd_on) {
                    /* 第二层：packed 直取线（携带 kfwd_p1）/链线（尾 lift
                     * 经 g->kfwd_pay 汇入——链 END 封块，lift 在链内做） */
                    kfwd_p2 = ir_PHI_2(IR_I64, kfwd_p1, g.kfwd_pay);
                    kfwd_p2t = ir_PHI_2(IR_I32, kfwd_p1t, g.kfwd_tag);
                }
                ir_ref ct = ir_END();
            ir_IF_FALSE(ifa);
                dimic_miss(&g, false);   /* 非数组 TypeError 走 helper 语义 */
                if (kfwd_on) {
                    /* 第三层 cf 输入：helper 成功写键槽后双 load */
                    kfwd_cf_pay = load_pay(j, g.dst);
                    kfwd_cf_tag = load_tag(j, g.dst);
                }
                ir_ref cf = ir_END();
                ir_MERGE_2(ct, cf);   /* 主尾活线，emit_seq 继续 */
                if (kfwd_on) {
                    /* 第三层：packed/链线（携带 kfwd_p2）/非数组线汇流
                     * → 挂起值（尾部 spec_kfwd_* 取此） */
                    kfwd_p3 = ir_PHI_2(IR_I64, kfwd_p2, kfwd_cf_pay);
                    kfwd_p3t = ir_PHI_2(IR_I32, kfwd_p2t, kfwd_cf_tag);
                }
            }
            }   /* spec_ht_ok else：generic/bound/链段结束 */
            /* 类型特化批1：配对 ADD 紧邻（spec_scan 证）→ 元素 TMP 身份
             * 传给 lower_binop 的 tb 检查点（消费即清，见 spec_bit_int）。
             * spec 作用域外/非配对情形不设。 */
            if (j->spec) { j->spec_tmp = op->ar; j->spec_tmp_valid = 1; }
            if (j->spec_chain) {
                /* 批4b-ii：phi 归属改道待发表——条目必挂（kind+idx 供消费
                 * 端 INT 证明），pay/unwritten 仅门过时给（phi 在
                 * spec_fwd_pay，emit_ht_fetch 写入）；旧单 pending 通道
                 * 恒关（spec_fwd 与 ctab 由 scan 模式分叉互斥，零串扰）。 */
                ctab_put(j, op->tr, op->ar, fwd_on ? j->spec_fwd_pay : 0,
                         fwd_on ? 1 : 0);
                j->spec_fwd_valid = 0;
            } else {
                /* 刀16：值直达挂起（emit 期已证 pc+1 配对 + 非 stop）——
                 * lower_binop 按 kind+idx 双等消费，消费即清；FETCH 尾若
                 * 未挂起则清位防上一次残留串用。 */
                j->spec_fwd_valid = fwd_on ? 1 : 0;
                if (fwd_on) { j->spec_fwd_kind = op->tr; j->spec_fwd_idx = op->ar; }
            }
            /* 批4b-i：key-fwd 挂起（F1 尾置、紧邻 F2 头消费即清；F2 自身
             * kfwd_on 恒 0——其 aop 是 ADD 非 FETCH——尾置 0 = 防残留，
             * 与 spec_fwd 同纪律）。 */
            j->spec_kfwd_valid = kfwd_on ? 1 : 0;
            if (kfwd_on) {
                j->spec_kfwd_idx = op->ar;
                j->spec_kfwd_pay = kfwd_p3;
                j->spec_kfwd_tag = kfwd_p3t;
            }
            pc++;
            break;
        }
        case OP_SEND_CAP: {  /* P0-18①：压捕获栈（上限 raise → deopt） */
            JIT_CTX();
            ir_ref sc[2] = { j->r_vm, slot_addr(j, op->a1) };
            ir_ref okc = ir_CALL_N(IR_I32,
                                   ir_CONST_ADDR((uintptr_t)tphp_jit_send_cap),
                                   2, sc);
            ir_ref ifsc = ir_IF(ir_EQ(okc, ir_CONST_I32(0)));
            ir_IF_TRUE(ifsc);
                ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
            ir_IF_FALSE(ifsc);
            pc++;
            break;
        }
        case OP_CLOSURE_INIT: {  /* 带捕获放开（P0-18①）：helper 弹 caps 栈 */
            JIT_CTX();
            ir_ref args[5] = { j->r_vm, slot_addr(j, op->ar),
                               ir_CONST_ADDR((uintptr_t)j->fn),
                               ir_CONST_U32(op->a1),
                               ir_CONST_I32((int32_t)op->a2) };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_closure_init), 5, args);
            pc++;
            break;
        }
        case OP_INIT_DYN: {
            /* 动态调用折叠：$f(...) —— 闭包槽 a1，SEND…DO_FCALL 同静态折叠 */
            if (j->p_callee || j->p_seq || j->p_new) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }  /* nested */
            j->p_dyn = 1;
            j->p_dyn_slot = op->a1;
            j->p_seq_pc = pc;   /* P1-12 批2-2：OSR deopt resume=序起点 */
            /* P0-15 批2: per-site fn 快照——ic 存闭包 fn 单字（1=未解析哨兵，
             * emit 一次性）。解释器 op_init_dyn 从不读 INIT_DYN 的 ic（勘察实证），
             * 与 INIT_FCALL ic&1 约定互不相干（per-op 字段）。 */
            j->p_dyn_ic = (uintptr_t)&op->ic;
            if (!op->ic) op->ic = (void *)1;
            j->p_argc = 0;
            pc++;
            break;
        }
        case OP_INIT_NEW: {
            /* P0-14 批2：new 序起点（回放 op_init_new→SENDs→NEW_USER）。
             * 批2-5 折叠判定点在 NEW_USER——类 ic 快照挂在 NEW_USER op 上
             * （op_new_user 自治解析填充），此处只起 p_seq 态；SEND 同样
             * 跳过（p_seq），折叠/回放在 NEW_USER 分流，两侧 SEND 语义
             * 完全一致（实参仅求值副作用，值留原槽由帧常规释放）。 */
            if (j->p_seq || j->p_new || j->p_callee || j->p_dyn || j->p_bfn) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            j->p_seq = 1;
            j->p_seq_pc = pc;
            j->p_argc = 0;   /* 批2-8: NEW 序 SEND 计数（ctor 折叠 argc 门） */
            pc++;
            break;
        }
        case OP_INIT_MCALL: {
            /* $o->m(...) 序起点：recv 快照进 entry scratch（实参表达式可
             * 复用接收者槽）；方法名限字面量（动态名 → fail 回退解释器）。
             * 批2-4/2-7 折叠直调：ic 已固化精确单态 user 方法
             * （init_mcall_core fill 规则：decl==o->cls && fn && vis 已验、
             * 非 native/非 magic）→ 命中即走 lower_call_finish 直调（同
             * INIT_FCALL 机械）；形态不支持（entry 未备/体含 replay op）
             * → p_seq=2 原序回放。守卫分模式（批2-7）：OSR 在本 op 发射，
             * 失配 RETURN OSR_SEQ_PC 从本 op 幂等重跑——实参尚未求值，无
             * 双执行；函数级后移到 DO_FCALL——实参表达式在本 op 之后发射
             * （compile.c A_METHODCALL），此处失配会跳过它们而 opseq 回放
             * 又跳过表达式 op（"JIT 已求值"）→ 陈旧实参；DO 处实参已由 IR
             * 无条件求值完，失配 → mfail 收集只重跑调用序，不重启整函数。 */
            if (j->p_seq || j->p_new || j->p_callee || j->p_dyn || j->p_bfn) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            if (op->tr != OPD_CONST || (op->t1 != OPD_CV && op->t1 != OPD_TMP)) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            ir_ref ra = slot_addr(j, op->a1);
            store_pay(j, j->r_call_clo, load_pay(j, ra));
            store_tag(j, j->r_call_clo, load_tag(j, ra));
            TMethod *sm = (TMethod *)op->ic;
            TFunc *mfn = sm ? sm->fn : NULL;
            /* 批2-7: a2==n_params 静态门——省略可选参/多传实参时直调布局
             * 不成立（缺参槽是 UNDEF 不是默认值、多参会覆写 callee 局部槽），
             * lower_call_finish L1873 会拒编整函数；不折叠走 p_seq=2 回放，
             * RECV 补默认值，编译覆盖不丢（回放 = 批2-7 前函数级行为）。 */
            if (mfn && sm->decl &&
                (mfn == j->fn || mfn->jit_entry) && jit_body_frameless(mfn) &&
                op->a2 == mfn->n_params) {
                j->p_seq_pc = pc;   /* 守卫 RETURN / mfail replay 读此值——先设 */
                if (j->osr) {
                    ir_ref gs = j->r_call_clo;
                    /* 守卫1：接收者必须是 T_OBJ（closure/标量/异常 getter 走慢路） */
                    ir_ref ift = ir_IF(ir_NE(load_tag(j, gs), ir_CONST_I32(T_OBJ)));
                    ir_IF_TRUE(ift);
                        ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));
                    ir_IF_FALSE(ift);
                    /* 守卫2：GC kind == OBJ（异常对象同为 T_OBJ） */
                    ir_ref ob = ir_LOAD_A(gs);
                    ir_ref ifk = ir_IF(ir_NE(
                        ir_LOAD_U8(ir_ADD_A(ob, ir_CONST_ADDR(offsetof(GCHead, kind)))),
                        ir_CONST_U8(GC_KIND_OBJ)));
                    ir_IF_TRUE(ifk);
                        ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));
                    ir_IF_FALSE(ifk);
                    /* 守卫3：精确单态 cls == ic->decl（镜像解释器快照 guard） */
                    ir_ref ifc = ir_IF(ir_NE(
                        ir_LOAD_A(ir_ADD_A(ob, ir_CONST_ADDR(offsetof(TObj, cls)))),
                        ir_CONST_ADDR((uintptr_t)sm->decl)));
                    ir_IF_TRUE(ifc);
                        ir_RETURN(ir_CONST_I32(OSR_SEQ_PC(j)));
                    ir_IF_FALSE(ifc);
                }
                j->p_callee = mfn;
                j->p_mcall = 1;
                j->p_mcall_cls = sm->decl;
                j->p_argc = 0;
                if (jit_dbg())
                    fprintf(stderr, "[jit] fold mcall %s pc=%u%s\n",
                            mfn->name ? tstr_val(mfn->name) : "?", pc,
                            j->osr ? " [osr]" : " [fn]");
                pc++;
                break;
            }
            j->p_seq = 2;
            j->p_seq_pc = pc;
            pc++;
            break;
        }
        case OP_NEW_USER: {
            /* 批2-5 折叠判定点（ic 在本 op 上）：OSR + 序态 + ic 固化解析
             * 成功 + 无 ctor + 非 abstract/非 Closure + 属性无 def_thunk →
             * alloc 零 raise 面直落（emit 检查保证：tm_malloc OOM=abort、
             * GC 扫描期 dtor 就地 fatal 不外传、无 VM thunk）；raise 型
             * site 由 threshold 前提排除（ic 由成功执行固化前的 raise 会
             * unwind 掉循环函数够不到 back-edge 阈值）。其余 → 原回放。 */
            TClass *ncls = (op->ic && op->ic != (void *)1) ? (TClass *)op->ic : NULL;
            /* 批2-7: 删 j->osr 门——判定全静态（ic 固化 + 非 abstract/
             * 非 Closure + 属性无 def_thunk）零运行时失败点、零 raise 面，
             * 函数级同享折叠；ic 未固化 → ncls NULL → 既有回放路径
             * （急切编译由 tphp_jit_defer_ic 延退到 ic 固化后）。 */
            bool nbase = j->p_seq == 1 && ncls &&
                         !(ncls->flags & CLS_ABSTRACT) && ncls != g_closure_cls;
            if (nbase) {
                for (int pi = 0; pi < ncls->n_props; pi++)
                    if (ncls->props[pi].def_thunk) { nbase = false; break; }
            }
            bool nfold = nbase && ncls->m_ctor < 0;
            /* 批2-8: user-ctor 折叠——静态门：fn 存在/非 abstract/非 native、
             * entry 就绪（或自递归）、argc==n_params（NEW 序 SEND 记槽）、
             * 有值 return 不折（generic move 掏空 slot0 读点，jit_ctor_ret_val）。
             * 可见性/abstract/Closure 由 ic 冻结前提排除（同批2-5 论证）；
             * 有 try 的 ctor 不可编译 → 无 jit_entry → 门自然落回放；
             * 不查 jit_body_frameless——f0 自框化让 replay 体可无帧直调
             * （ctor 普遍含属性写，查了就全军覆没）。 */
            TFunc *cfn = NULL;
            bool cfold = false;
            if (nbase && ncls->m_ctor >= 0) {
                TMethod *cm = &ncls->methods[ncls->m_ctor];
                cfn = cm->fn;
                cfold = cfn && !cm->is_abstract && !cm->native &&
                        (cfn == j->fn || cfn->jit_entry) &&
                        !jit_ctor_ret_val(cfn) &&
                        j->p_argc == cfn->n_params;
            }
            if (nfold) {
                j->p_new = 1;
                j->p_new_cls = ncls;
                if (jit_dbg())
                    fprintf(stderr, "[jit] fold new %s pc=%u%s\n",
                            tstr_val(ncls->name), pc,
                            j->osr ? " [osr]" : " [fn]");
                lower_new_finish(j, op);
                pc++;
                break;
            }
            if (cfold) {
                if (jit_dbg())
                    fprintf(stderr, "[jit] fold new-ctor %s pc=%u%s\n",
                            tstr_val(ncls->name), pc,
                            j->osr ? " [osr]" : " [fn]");
                lower_new_ctor_finish(j, op, cfn, ncls);
                if (j->fail) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                if (j->mfail_n) {
                    /* 函数级守卫失配路（同 OP_DO_FCALL 聚合）：成功路 END
                     * 暂存 → MERGE_N 回放重跑调用序（表达式已求值、opseq
                     * 跳过恰正确）→ 与回放成功路合流续主序。 */
                    ir_ref esucc = ir_END();
                    ir_MERGE_N((ir_ref)j->mfail_n, j->mfail);
                    j->mfail_n = 0;
                    lower_replay(j, j->p_seq_pc, pc + 1);
                    ir_ref ereplay = ir_END();
                    ir_MERGE_2(ereplay, esucc);
                }
                pc++;
                break;
            }
            if (j->p_seq != 1) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            lower_replay(j, j->p_seq_pc, pc + 1);
            pc++;
            break;
        }
        case OP_FETCH_PROP: {
            /* prop v1.5 批1（PLAN/prop-ic.md §2.1）：CONST 名 + helper 开
             * → IR 内联快路（镜像解释器 P0-19 round-6 IC 快路）；miss 落
             * 既有 helper。开关关/动态名/helper 关 → HEAD 路径零改动。 */
            if (prop_inline_enabled() && prop_helper_enabled()
                && op->t2 == OPD_CONST
                && j->fn->lits[op->a2].t == T_STR) {
                JIT_CTX();
                TStr *namep = (TStr *)j->fn->lits[op->a2].u.p;
                ir_ref name_c = ir_CONST_ADDR((uintptr_t)namep);
                ir_ref obj_a = slot_addr(j, op->a1);
                ir_ref obj_t = load_tag(j, obj_a);
                ir_ref ends[6];
                int ne = 0;

                /* L1: tag==T_OBJ */
                ir_ref if1 = ir_IF(ir_EQ(obj_t, ir_CONST_I32(T_OBJ)));
                ir_IF_TRUE(if1);
                    ir_ref obj_p = load_pay(j, obj_a);
                    ir_ref kind = ir_LOAD_U8(ir_ADD_A(obj_p,
                        ir_CONST_ADDR(offsetof(GCHead, kind))));
                    /* L2: kind==GC_KIND_OBJ */
                    ir_ref if2 = ir_IF(ir_EQ(kind, ir_CONST_U8(GC_KIND_OBJ)));
                    ir_IF_TRUE(if2);
                        /* load ic (slot+1)、cls、np — 全部安全 */
                        ir_ref ic_raw = ir_LOAD_I64(
                            ir_CONST_ADDR((uintptr_t)&op->ic));
                        ir_ref cls = ir_LOAD_A(ir_ADD_A(obj_p,
                            ir_CONST_ADDR(offsetof(TObj, cls))));
                        ir_ref np32 = ir_LOAD_I32(ir_ADD_A(cls,
                            ir_CONST_ADDR(offsetof(TClass, n_props))));
                        ir_ref slot = ir_SUB_I64(ic_raw, ir_CONST_I64(1));
                        ir_ref np64 = ir_ZEXT(IR_I64, np32);
                        /* L3: ic 已填 && slot < np（合并——均只需已载值） */
                        ir_ref ok3 = ir_AND(IR_BOOL,
                            ir_NE(ic_raw, ir_CONST_I64(0)),
                            ir_LT(slot, np64));
                        ir_ref if3 = ir_IF(ok3);
                        ir_IF_TRUE(if3);
                            /* load pp/pname/vis/hooks — slot 已证合法 */
                            ir_ref props_arr = ir_LOAD_A(ir_ADD_A(cls,
                                ir_CONST_ADDR(offsetof(TClass, props))));
                            ir_ref pp = ir_ADD_A(props_arr,
                                ir_MUL_I64(slot, ir_CONST_I64(sizeof(TProp))));
                            ir_ref pname = ir_LOAD_A(pp);  /* TProp.name @0 */
                            ir_ref vis = ir_LOAD_U8(ir_ADD_A(pp,
                                ir_CONST_ADDR(offsetof(TProp, visibility))));
                            ir_ref hooks = ir_LOAD_U8(ir_ADD_A(pp,
                                ir_CONST_ADDR(offsetof(TProp, has_hooks))));
                            /* L4: 名指针相等 + PUBLIC + !hooks */
                            ir_ref ok4 = ir_AND(IR_BOOL,
                                ir_AND(IR_BOOL,
                                    ir_EQ(pname, name_c),
                                    ir_EQ(vis, ir_CONST_U8(CVIS_PUBLIC))),
                                ir_EQ(hooks, ir_CONST_U8(0)));
                            ir_ref if4 = ir_IF(ok4);
                            ir_IF_TRUE(if4);
                                /* ---- HIT：镜像解释器 4214-4218 ---- */
                                ir_ref vaddr = ir_ADD_A(
                                    ir_LOAD_A(ir_ADD_A(obj_p,
                                        ir_CONST_ADDR(offsetof(TObj, props)))),
                                    ir_MUL_I64(slot, ir_CONST_I64(TVAL_SZ)));
                                /* addref v（dimic_take 同款菱形） */
                                ir_ref vtag = ir_LOAD_I32(
                                    ir_ADD_A(vaddr, ir_CONST_ADDR(TVAL_TAG)));
                                ir_ref ifv = ir_IF(ir_AND(IR_BOOL,
                                    ir_GE(vtag, ir_CONST_I32(T_STR)),
                                    ir_LE(vtag, ir_CONST_I32(T_CLOSURE))));
                                ir_IF_TRUE(ifv);
                                    ir_ref gcv = ir_LOAD_A(vaddr);
                                    ir_ref fv = ir_ZEXT(IR_I32,
                                        ir_LOAD_U8(ir_ADD_A(gcv,
                                            ir_CONST_ADDR(offsetof(GCHead, flags)))));
                                    ir_ref ifiv = ir_IF(ir_EQ(
                                        ir_AND_I32(fv,
                                            ir_CONST_I32(GC_FLAG_INTERNED)),
                                        ir_CONST_I32(0)));
                                    ir_IF_TRUE(ifiv);
                                        ir_ref rcv = ir_LOAD_I32(gcv);
                                        ir_STORE(gcv,
                                            ir_ADD_I32(rcv, ir_CONST_I32(1)));
                                        ir_ref eV1 = ir_END();
                                    ir_IF_FALSE(ifiv);
                                        ir_ref eV0 = ir_END();
                                    ir_MERGE_2(eV1, eV0);
                                    ir_ref eRefV = ir_END();
                                ir_IF_FALSE(ifv);
                                    ir_ref eScV = ir_END();
                                ir_MERGE_2(eRefV, eScV);
                                /* wtake：UNUSED=释 src；否则释 dst + 16B store */
                                if (op->tr == OPD_UNUSED) {
                                    ir_ref tr[1] = { vaddr };
                                    ir_CALL_N(IR_VOID,
                                        ir_CONST_ADDR((uintptr_t)tval_release),
                                        1, tr);
                                } else {
                                    ir_ref dst = slot_addr(j, op->ar);
                                    if (dst_gate_needed(j, op))
                                        rel_dst_gate(j, dst);
                                    store_pay(j, dst, ir_LOAD_I64(vaddr));
                                    store_tag(j, dst, vtag);
                                }
                                ends[ne++] = ir_END();  /* HIT */
                            ir_IF_FALSE(if4);
                                /* MISS：helper */
                                ir_ref t5[5] = { j->r_vm,
                                    ir_CONST_ADDR((uintptr_t)j->fn),
                                    ir_CONST_ADDR((uintptr_t)op),
                                    ir_CONST_I32((int32_t)pc),
                                    j->r_slots };
                                ir_ref ok = ir_CALL_N(IR_I32,
                                    ir_CONST_ADDR((uintptr_t)tphp_jit_fetch_prop),
                                    5, t5);
                                ir_ref ifp = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
                                ir_IF_TRUE(ifp);
                                    ir_RETURN(ir_CONST_I32(
                                        j->osr ? (int32_t)pc : 0));
                                ir_IF_FALSE(ifp);
                                ends[ne++] = ir_END();  /* helper ok */
                            /* MERGE hit/helper → 这是 if3-TRUE 的出口 */
                            ir_ref e4t = ends[ne - 2], e4f = ends[ne - 1];
                            ne -= 2;
                            ir_MERGE_2(e4t, e4f);
                            ends[ne++] = ir_END();  /* 关闭 if3-TRUE */
                        ir_IF_FALSE(if3);
                            /* ic 未填 / OOB → helper */
                            ir_ref t3[5] = { j->r_vm,
                                ir_CONST_ADDR((uintptr_t)j->fn),
                                ir_CONST_ADDR((uintptr_t)op),
                                ir_CONST_I32((int32_t)pc),
                                j->r_slots };
                            ir_ref ok3b = ir_CALL_N(IR_I32,
                                ir_CONST_ADDR((uintptr_t)tphp_jit_fetch_prop),
                                5, t3);
                            ir_ref ifp3 = ir_IF(ir_EQ(ok3b, ir_CONST_I32(0)));
                            ir_IF_TRUE(ifp3);
                                ir_RETURN(ir_CONST_I32(
                                    j->osr ? (int32_t)pc : 0));
                            ir_IF_FALSE(ifp3);
                            ends[ne++] = ir_END();  /* if3-FALSE */
                        /* MERGE if3 T/F → 关闭 if2-TRUE */
                        {
                            ir_ref a = ends[ne - 2], b = ends[ne - 1];
                            ne -= 2;
                            ir_MERGE_2(a, b);
                            ends[ne++] = ir_END();
                        }
                    ir_IF_FALSE(if2);
                        /* 非 GC_KIND_OBJ → helper */
                        ir_ref t2[5] = { j->r_vm,
                            ir_CONST_ADDR((uintptr_t)j->fn),
                            ir_CONST_ADDR((uintptr_t)op),
                            ir_CONST_I32((int32_t)pc),
                            j->r_slots };
                        ir_ref ok2 = ir_CALL_N(IR_I32,
                            ir_CONST_ADDR((uintptr_t)tphp_jit_fetch_prop),
                            5, t2);
                        ir_ref ifp2 = ir_IF(ir_EQ(ok2, ir_CONST_I32(0)));
                        ir_IF_TRUE(ifp2);
                            ir_RETURN(ir_CONST_I32(
                                j->osr ? (int32_t)pc : 0));
                        ir_IF_FALSE(ifp2);
                        ends[ne++] = ir_END();  /* if2-FALSE */
                    /* MERGE if2 T/F → 关闭 if1-TRUE */
                    {
                        ir_ref a = ends[ne - 2], b = ends[ne - 1];
                        ne -= 2;
                        ir_MERGE_2(a, b);
                        ends[ne++] = ir_END();
                    }
                ir_IF_FALSE(if1);
                    /* 非 T_OBJ → helper */
                    ir_ref t1[5] = { j->r_vm,
                        ir_CONST_ADDR((uintptr_t)j->fn),
                        ir_CONST_ADDR((uintptr_t)op),
                        ir_CONST_I32((int32_t)pc),
                        j->r_slots };
                    ir_ref ok1 = ir_CALL_N(IR_I32,
                        ir_CONST_ADDR((uintptr_t)tphp_jit_fetch_prop),
                        5, t1);
                    ir_ref ifp1 = ir_IF(ir_EQ(ok1, ir_CONST_I32(0)));
                    ir_IF_TRUE(ifp1);
                        ir_RETURN(ir_CONST_I32(
                            j->osr ? (int32_t)pc : 0));
                    ir_IF_FALSE(ifp1);
                    ends[ne++] = ir_END();  /* if1-FALSE */
                /* 最终 MERGE：if1 T/F 两条路径 */
                {
                    ir_ref a = ends[ne - 2], b = ends[ne - 1];
                    ne -= 2;
                    ir_MERGE_2(a, b);
                }
                pc++;
                break;
            }
            /* HEAD 路径：helper 或 replay */
            if (!prop_helper_enabled()) {
                lower_replay(j, pc, pc + 1);
                pc++;
                break;
            }
            JIT_CTX();
            ir_ref t[5] = { j->r_vm,
                            ir_CONST_ADDR((uintptr_t)j->fn),
                            ir_CONST_ADDR((uintptr_t)op),
                            ir_CONST_I32((int32_t)pc),
                            j->r_slots };
            ir_ref ok = ir_CALL_N(IR_I32,
                                  ir_CONST_ADDR((uintptr_t)tphp_jit_fetch_prop),
                                  5, t);
            ir_ref ifp = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
            ir_IF_TRUE(ifp);
                ir_RETURN(ir_CONST_I32(j->osr ? (int32_t)pc : 0));
            ir_IF_FALSE(ifp);
            pc++;
            break;
        }
        case OP_ASSIGN_PROP: {
            /* prop v1.5 批2（PLAN/prop-ic.md §2.2）：CONST 名 → IR 内联
             * 快路（镜像解释器 3119-3138）；miss/dynamic/关 → HEAD replay。 */
            if (prop_inline_enabled()
                && op->t2 == OPD_CONST
                && j->fn->lits[op->a2].t == T_STR) {
                JIT_CTX();
                TStr *namep = (TStr *)j->fn->lits[op->a2].u.p;
                ir_ref name_c = ir_CONST_ADDR((uintptr_t)namep);
                ir_ref obj_a = slot_addr(j, op->a1);
                ir_ref obj_t = load_tag(j, obj_a);
                /* 值源：tr/ar = o3（镜像 rdv(op,3)）；写回恒 slots[ar] */
                ir_ref ends[8];
                int ne = 0;

                /* L1: tag==T_OBJ */
                ir_ref if1 = ir_IF(ir_EQ(obj_t, ir_CONST_I32(T_OBJ)));
                ir_IF_TRUE(if1);
                    ir_ref obj_p = load_pay(j, obj_a);
                    ir_ref kind = ir_LOAD_U8(ir_ADD_A(obj_p,
                        ir_CONST_ADDR(offsetof(GCHead, kind))));
                    /* L2: kind==GC_KIND_OBJ */
                    ir_ref if2 = ir_IF(ir_EQ(kind, ir_CONST_U8(GC_KIND_OBJ)));
                    ir_IF_TRUE(if2);
                        ir_ref ic_raw = ir_LOAD_I64(
                            ir_CONST_ADDR((uintptr_t)&op->ic));
                        ir_ref cls = ir_LOAD_A(ir_ADD_A(obj_p,
                            ir_CONST_ADDR(offsetof(TObj, cls))));
                        ir_ref np32 = ir_LOAD_I32(ir_ADD_A(cls,
                            ir_CONST_ADDR(offsetof(TClass, n_props))));
                        ir_ref slot = ir_SUB_I64(ic_raw, ir_CONST_I64(1));
                        ir_ref np64 = ir_ZEXT(IR_I64, np32);
                        /* L3: ic 已填 && slot < np */
                        ir_ref ok3 = ir_AND(IR_BOOL,
                            ir_NE(ic_raw, ir_CONST_I64(0)),
                            ir_LT(slot, np64));
                        ir_ref if3 = ir_IF(ok3);
                        ir_IF_TRUE(if3);
                            ir_ref props_arr = ir_LOAD_A(ir_ADD_A(cls,
                                ir_CONST_ADDR(offsetof(TClass, props))));
                            ir_ref pp = ir_ADD_A(props_arr,
                                ir_MUL_I64(slot, ir_CONST_I64(sizeof(TProp))));
                            ir_ref pname = ir_LOAD_A(pp);  /* TProp.name @0 */
                            /* L4+L5：名指针 + PUBLIC + !set_vis + !hooks
                             * + !type + !is_readonly + !(cls.flags&CLS_READONLY)
                             * （镜像解释器 3124-3126） */
                            ir_ref vis = ir_LOAD_U8(ir_ADD_A(pp,
                                ir_CONST_ADDR(offsetof(TProp, visibility))));
                            ir_ref setv = ir_LOAD_U8(ir_ADD_A(pp,
                                ir_CONST_ADDR(offsetof(TProp, set_visibility))));
                            ir_ref hooks = ir_LOAD_U8(ir_ADD_A(pp,
                                ir_CONST_ADDR(offsetof(TProp, has_hooks))));
                            ir_ref ptype = ir_LOAD_A(ir_ADD_A(pp,
                                ir_CONST_ADDR(offsetof(TProp, type))));
                            ir_ref ro = ir_LOAD_U8(ir_ADD_A(pp,
                                ir_CONST_ADDR(offsetof(TProp, is_readonly))));
                            ir_ref cflags = ir_LOAD_U8(ir_ADD_A(cls,
                                ir_CONST_ADDR(offsetof(TClass, flags))));
                            ir_ref ok4 = ir_AND(IR_BOOL,
                                ir_AND(IR_BOOL,
                                    ir_AND(IR_BOOL,
                                        ir_EQ(pname, name_c),
                                        ir_EQ(vis, ir_CONST_U8(CVIS_PUBLIC))),
                                    ir_AND(IR_BOOL,
                                        ir_EQ(setv, ir_CONST_U8(0)),
                                        ir_EQ(hooks, ir_CONST_U8(0)))),
                                ir_AND(IR_BOOL,
                                    ir_AND(IR_BOOL,
                                        ir_EQ(ptype, ir_CONST_ADDR(0)),
                                        ir_EQ(ro, ir_CONST_U8(0))),
                                    ir_EQ(ir_AND_I32(
                                        ir_ZEXT(IR_I32, cflags),
                                        ir_CONST_I32(CLS_READONLY)),
                                        ir_CONST_I32(0))));
                            ir_ref if4 = ir_IF(ok4);
                            ir_IF_TRUE(if4);
                                /* ---- HIT：镜像解释器 3127-3136 ---- */
                                /* 取 val：CONST→lits 常量 / 否则 slots[ar] */
                                ir_ref vt, vp;
                                if (op->tr == OPD_CONST) {
                                    TVal cv = j->fn->lits[op->ar];
                                    vt = ir_CONST_I32((int32_t)cv.t);
                                    vp = ir_CONST_I64(cv.u.i);
                                } else {
                                    ir_ref va = slot_addr(j, op->ar);
                                    vt = load_tag(j, va);
                                    vp = load_pay(j, va);
                                }
                                /* addref(val) — tag∈[T_STR,T_CLOSURE] 门 */
                                ir_ref ifv = ir_IF(ir_AND(IR_BOOL,
                                    ir_GE(vt, ir_CONST_I32(T_STR)),
                                    ir_LE(vt, ir_CONST_I32(T_CLOSURE))));
                                ir_IF_TRUE(ifv);
                                    ir_ref gcv = vp;
                                    ir_ref fv = ir_ZEXT(IR_I32,
                                        ir_LOAD_U8(ir_ADD_A(gcv,
                                            ir_CONST_ADDR(offsetof(GCHead,
                                                flags)))));
                                    ir_ref ifiv = ir_IF(ir_EQ(
                                        ir_AND_I32(fv,
                                            ir_CONST_I32(GC_FLAG_INTERNED)),
                                        ir_CONST_I32(0)));
                                    ir_IF_TRUE(ifiv);
                                        ir_ref rcv = ir_LOAD_I32(gcv);
                                        ir_STORE(gcv,
                                            ir_ADD_I32(rcv,
                                                ir_CONST_I32(1)));
                                        ir_ref eV1 = ir_END();
                                    ir_IF_FALSE(ifiv);
                                        ir_ref eV0 = ir_END();
                                    ir_MERGE_2(eV1, eV0);
                                    ir_ref eRefV = ir_END();
                                ir_IF_FALSE(ifv);
                                    ir_ref eScV = ir_END();
                                ir_MERGE_2(eRefV, eScV);
                                /* 释旧 prop + 16B store */
                                ir_ref prop_a = ir_ADD_A(
                                    ir_LOAD_A(ir_ADD_A(obj_p,
                                        ir_CONST_ADDR(offsetof(TObj,
                                            props)))),
                                    ir_MUL_I64(slot,
                                        ir_CONST_I64(TVAL_SZ)));
                                ir_ref trp[1] = { prop_a };
                                ir_CALL_N(IR_VOID,
                                    ir_CONST_ADDR((uintptr_t)tval_release),
                                    1, trp);
                                ir_STORE(prop_a, vp);
                                ir_STORE(ir_ADD_A(prop_a,
                                    ir_CONST_ADDR(TVAL_TAG)), vt);
                                /* 表达式结果写回 slots[ar]（tr!=UNUSED） */
                                if (op->tr != OPD_UNUSED) {
                                    ir_ref da = slot_addr(j, op->ar);
                                    /* addref(val) again */
                                    ir_ref ifv2 = ir_IF(ir_AND(IR_BOOL,
                                        ir_GE(vt, ir_CONST_I32(T_STR)),
                                        ir_LE(vt,
                                            ir_CONST_I32(T_CLOSURE))));
                                    ir_IF_TRUE(ifv2);
                                        ir_ref gcv2 = vp;
                                        ir_ref fv2 = ir_ZEXT(IR_I32,
                                            ir_LOAD_U8(ir_ADD_A(gcv2,
                                                ir_CONST_ADDR(
                                                    offsetof(GCHead,
                                                        flags)))));
                                        ir_ref ifiv2 = ir_IF(ir_EQ(
                                            ir_AND_I32(fv2,
                                                ir_CONST_I32(
                                                    GC_FLAG_INTERNED)),
                                            ir_CONST_I32(0)));
                                        ir_IF_TRUE(ifiv2);
                                            ir_ref rcv2 =
                                                ir_LOAD_I32(gcv2);
                                            ir_STORE(gcv2,
                                                ir_ADD_I32(rcv2,
                                                    ir_CONST_I32(1)));
                                            ir_ref eV1b = ir_END();
                                        ir_IF_FALSE(ifiv2);
                                            ir_ref eV0b = ir_END();
                                        ir_MERGE_2(eV1b, eV0b);
                                        ir_ref eRefV2 = ir_END();
                                    ir_IF_FALSE(ifv2);
                                        ir_ref eScV2 = ir_END();
                                    ir_MERGE_2(eRefV2, eScV2);
                                    /* 释 slots[ar] 旧值 + 16B store */
                                    rel_dst_gate(j, da);
                                    ir_STORE(da, vp);
                                    ir_STORE(ir_ADD_A(da,
                                        ir_CONST_ADDR(TVAL_TAG)), vt);
                                }
                                ends[ne++] = ir_END();  /* HIT */
                            ir_IF_FALSE(if4);
                                /* L4/L5 fail → replay（回填 ic） */
                                lower_replay(j, pc, pc + 1);
                                ends[ne++] = ir_END();  /* if4-FALSE */
                            /* MERGE if4 T/F → 关闭 if3-TRUE */
                            {
                                ir_ref a = ends[ne - 2], b = ends[ne - 1];
                                ne -= 2;
                                ir_MERGE_2(a, b);
                                ends[ne++] = ir_END();
                            }
                        ir_IF_FALSE(if3);
                            /* ic 未填 / OOB → replay */
                            lower_replay(j, pc, pc + 1);
                            ends[ne++] = ir_END();  /* if3-FALSE */
                        /* MERGE if3 T/F → 关闭 if2-TRUE */
                        {
                            ir_ref a = ends[ne - 2], b = ends[ne - 1];
                            ne -= 2;
                            ir_MERGE_2(a, b);
                            ends[ne++] = ir_END();
                        }
                    ir_IF_FALSE(if2);
                        lower_replay(j, pc, pc + 1);
                        ends[ne++] = ir_END();  /* if2-FALSE */
                    /* MERGE if2 T/F → 关闭 if1-TRUE */
                    {
                        ir_ref a = ends[ne - 2], b = ends[ne - 1];
                        ne -= 2;
                        ir_MERGE_2(a, b);
                        ends[ne++] = ir_END();
                    }
                ir_IF_FALSE(if1);
                    lower_replay(j, pc, pc + 1);
                    ends[ne++] = ir_END();  /* if1-FALSE */
                /* 最终 MERGE：if1 T/F */
                {
                    ir_ref a = ends[ne - 2], b = ends[ne - 1];
                    ne -= 2;
                    ir_MERGE_2(a, b);
                }
                pc++;
                break;
            }
            /* HEAD 路径：replay（真 handler 回填 ic） */
            lower_replay(j, pc, pc + 1);
            pc++;
            break;
        }
        case OP_ISSET_PROP: case OP_UNSET_PROP: {
            /* P0-14 批2-2：单 op pad 执行——真 handler 读自身操作数（动态
             * 名/槽皆活在本位）+ 写 dst 槽；magic __get/__set 与 raise 走
             * tphp_jit_opseq 同款 deopt 门。p_seq 内的实参表达式形态同样
             * 适用（本位先行求值，序回放跳过它）。 */
            lower_replay(j, pc, pc + 1);
            pc++;
            break;
        }
        case OP_FE_RESET: case OP_FE_FETCH: {
            /* P0-14 批5：单 op pad 执行；FE_FETCH 即回跳目标=循环头，
             * 其后 JMPZ 由既有 loopcond 分支消费（形态零适配）。
             * 挡板（嵌套 loop×FE）：FE_RESET 必在函数级（loop_sp==0）、
             * FE_FETCH 必在自身循环头（sp==1 且 hdr==pc）——被外层 loop
             * 包裹的 foreach 此前 O1 下崩（tv_copy 实参 src=0，O0 全对 =
             * coalesce/live_ranges/reg_alloc 链残余，批3 emit_arguments 同
             * 族第二形态）→ fail 回退解释器，根修后放开（backlog P0-14）。 */
            if (op->op == OP_FE_RESET ? j->loop_sp != 0
                                      : !(j->loop_sp == 1 && j->loop_hdr[0] == pc)) {
                j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
            }
            lower_replay(j, pc, pc + 1);
            pc++;
            break;
        }
        case OP_ISSET_DIM: {
            /* P0-14 批4：单 op pad 执行——真 handler raw read 操作数
             * （base/key 任意槽形态含 CONST 皆自读）写 dst bool 槽 */
            lower_replay(j, pc, pc + 1);
            pc++;
            break;
        }
        case OP_SUPPRESS_ON:
        case OP_SUPPRESS_OFF: {
            /* @ 抑制（11-10）：直接写 vm.x.silence（load/add|sub/store 三
             * IR，与 handler 语义逐条同构；OFF 防负走 clamp 菱形）。 */
            JIT_CTX();
            ir_ref sa = ir_ADD_A(j->r_vm,
                ir_CONST_ADDR(offsetof(VM, x) + offsetof(TExecCtx, silence)));
            ir_ref v = ir_LOAD_I32(sa);
            ir_ref nv = (op->op == OP_SUPPRESS_ON)
                ? ir_ADD_I32(v, ir_CONST_I32(1))
                : ir_SUB_I32(v, ir_CONST_I32(1));
            if (op->op == OP_SUPPRESS_ON) {
                ir_STORE(sa, nv);
            } else {
                ir_ref ifn = ir_IF(ir_LT(nv, ir_CONST_I32(0)));
                ir_IF_TRUE(ifn);
                ir_STORE(sa, ir_CONST_I32(0));
                ir_ref eA = ir_END();
                ir_IF_FALSE(ifn);
                ir_STORE(sa, nv);
                ir_ref eB = ir_END();
                ir_MERGE_2(eA, eB);
            }
            pc++;
            break;
        }
        case OP_UNSET_CV: {
            /* 解释器 op_unset_cv 同款：释放槽值 + 置 UNDEF（dtor defer 于
             * mem.c，release 不进用户码不 raise → 无需 deopt 门） */
            if (op->t1 != OPD_CV) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            ir_ref da = slot_addr(j, op->a1);
            ir_ref tr1[1] = { da };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tval_release), 1, tr1);
            store_imm(j, da, (int32_t)T_UNDEF, ir_CONST_I64(0));
            pc++;
            break;
        }
        case OP_COPY: case OP_ASSIGN:
        case OP_BOOL_NOT: case OP_BOOL_VAL: case OP_NEG: case OP_POS:
        case OP_CAST:
            lower_un(j, op); pc++; break;
        case OP_INC: case OP_DEC:
            lower_incdec(j, op, pc); pc++; break;
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_BW_AND: case OP_BW_OR: case OP_BW_XOR:
        case OP_DIV: case OP_MOD: case OP_SHL: case OP_SHR:
            lower_binop(j, op);
            /* 批4b-ii：链加尾挂表供下一加免槽 load——pay 仅当快线跑过
             * （spec_ctab_sum 非 0：恒 INT 成立）+ 消费端存在且块连续
             * （chain_consumer_pc）+ fetch_fwd_enabled 总闸；条目恒挂
             * （tr==TMP 的链加 dst 必有消费端引用 = scan 证），pay=0 时
             * 只作 INT 证明、消费端槽 load（链 dst 每迭代 store 恒有值）。 */
            if (j->spec && j->spec_chain && op->tr == OPD_TMP &&
                j->spec_ctab_sum) {
                ir_ref csum = j->spec_ctab_sum;
                int cpay = fetch_fwd_enabled() &&
                           chain_consumer_pc(j->fn, pc, OPD_TMP, op->ar,
                                             stops, nstops) != 0;
                ctab_put(j, OPD_TMP, op->ar, cpay ? csum : 0, 0);
            }
            pc++; break;
        case OP_CONCAT: {
            /* P0-14：镜像 op_concat——tphp_jit_concat 快慢路（双 STR/标量
             * 零 setjmp；OBJ 等冷路 setjmp wrapper），raise → deopt
             * （P0-12 IF_TRUE RETURN / IF_FALSE 留开姿势） */
            ir_ref dstA = slot_addr(j, op->ar);
            ir_ref aA = slot_addr(j, op->a1);
            ir_ref bA;
            if (op->t2 == OPD_CONST) {
                TVal kv = j->fn->lits[op->a2];
                bA = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
                store_imm(j, bA, (int32_t)kv.t, ir_CONST_I64(kv.u.i));
            } else {
                bA = slot_addr(j, op->a2);
            }
            int inplace = (op->t1 == op->tr && op->a1 == op->ar) ? 1 : 0;
            ir_ref t[5] = { j->r_vm, dstA, aA, bA, ir_CONST_I32(inplace) };
            ir_ref ok = ir_CALL_N(IR_I32,
                                  ir_CONST_ADDR((uintptr_t)tphp_jit_concat), 5, t);
            ir_ref ifc = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
            ir_IF_TRUE(ifc);
                ir_RETURN(ir_CONST_I32(OSR_RET_PC(j)));
            ir_IF_FALSE(ifc);
            pc++;
            break;
        }
        case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE:
        case OP_LT: case OP_LE: case OP_CMP:
            lower_cmp(j, op); pc++; break;
        case OP_RETURN:
            lower_return(j, op);
            return trap_ret();
        case OP_INIT_FCALL: {
            /* start folded call; arg expressions run as normal ops until
             * SEND_vals then DO_FCALL (PLAN/ir-jit.md section 6) */
            if (j->p_callee || j->p_bfn || j->p_seq || j->p_new || j->p_dyn) {
                /* 刀5C：嵌套调用（外层折叠序的参数表达式内含内层调用）。
                 * 保守门（防双执行/状态错乱）：
                 *  ①仅函数级（OSR chunk 的 resume pc 语义不覆盖内层序）
                 *  ②外层须为普通静态/内建折叠序（p_seq/p_new/p_dyn 回放
                 *    或动态调用态不放行）
                 *  ③内层必须是已解析的 builtin（ic&1）——用户函数内层调用
                 *    可能带副作用，raise→函数级 deopt 整函数重跑会双执行
                 *  ④多层嵌套（nest_active 已置位）拒编
                 * 命中即记 nest_start，内层 INIT/SEND/DO 交给 opseq 真
                 * handler 回放（vm.c 派发表已加 OP_INIT_FCALL），表达式 op
                 * 照常 IR emit（opseq 跳过表达式 op，零双跑）。 */
                if (nest_call_enabled() && !j->osr && !j->nest_active
                    && !j->p_seq && !j->p_new && !j->p_dyn
                    && (j->p_callee || j->p_bfn)
                    && op->ic && ((uintptr_t)op->ic & 1)) {
                    j->nest_active = 1;
                    j->nest_start = pc;
                    pc++;
                    break;
                }
                j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);  /* nested */
            }
            if ((uintptr_t)op->ic & 1)
                j->p_bfn = (void *)((uintptr_t)op->ic & ~(uintptr_t)1);
            else
                j->p_callee = (TFunc *)op->ic;
            j->p_dyn = 0;
            j->p_seq_pc = pc;   /* P1-12 批2-2：OSR deopt resume=序起点 */
            j->p_argc = 0;
            pc++;
            break;
        }
        case OP_INIT_SMCALL: {
            /* 批3-3：静态方法直调——ic 已由 eligibility 解析缓存（TFunc*，
             * 调用方 jit_smcallee）。p_seq 回放序列内不直调（pc 跳过，真
             * handler 在 opseq 派发表执行）；嵌套任何调用态即 fail。 */
            if (j->p_new) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }  /* 批2-5: 无回放可依 */
            if (j->p_seq) { pc++; break; }
            if (j->p_callee || j->p_bfn || j->p_dyn || j->p_sm || !op->ic ||
                ((uintptr_t)op->ic & 1)) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            j->p_callee = (TFunc *)op->ic;
            j->p_sm = 1;
            j->p_argc = 0;
            pc++;
            break;
        }
        case OP_SEND_VAL: case OP_SEND_VAR: {
            if (j->nest_active) { pc++; break; }   /* 刀5C：内层回放态——实参由
                                                     * 真 handler 读槽，编译期不记
                                                     * 外层 p_arg_slot（防污染） */
            if (!j->p_callee && !j->p_dyn && !j->p_bfn && !j->p_seq && !j->p_new) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            if (j->p_seq) {
                /* 批2-8: NEW 序（==1）同时记槽——ctor 折叠在 NEW_USER 才知
                 * 类，argc 门需要 SEND 计数；mcall 序回放（==2）照旧不记。 */
                if (j->p_seq == 1 && j->p_argc < 9)
                    j->p_arg_slot[j->p_argc++] = ((uint32_t)op->t1 << 24) | op->a1;
                pc++; break;
            }
            if (j->p_new) { pc++; break; }   /* 批2-5: 无 ctor 不消费实参——仅求值
                                              * 副作用（表达式 op 照常发射），
                                              * 值留原槽由帧常规释放 */
            if (j->p_argc >= 9) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            j->p_arg_slot[j->p_argc++] = ((uint32_t)op->t1 << 24) | op->a1;
            pc++;
            break;
        }
        case OP_DO_FCALL: {
            if (j->nest_active) {
                /* 刀5C：回放内层 INIT..DO（真 handler 完整语义，含
                 * builtin raise/deopt 上传）；结果写内层 dst 槽，外层折叠
                 * 序后续 SEND 引用该槽。lower_replay 内 p_seq=0 对本态
                 * 无影响（本态从不设 p_seq）。 */
                lower_replay(j, j->nest_start, pc + 1);
                j->nest_active = 0;
                pc++;
                break;
            }
            if (j->p_seq == 2) {   /* P0-14 批2: mcall 序回放 */
                lower_replay(j, j->p_seq_pc, pc + 1);
                pc++;
                break;
            }
            if (!j->p_callee && !j->p_dyn && !j->p_bfn) { j->fail = 1; do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0); }
            lower_call_finish(j, op);
            if (j->fail) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
            if (j->mfail_n) {
                /* 批2-7: 函数级 mcall 守卫失配路——折叠成功路 END 暂存，
                 * 失配块 MERGE_N 聚合走 lower_replay 只重跑 INIT..DO 调用
                 * 序（实参已由 IR 求值、opseq 跳过表达式 op 恰正确；recv
                 * 用 r_call_clo 快照 = INIT 时值，PHP 语义本就只求值接收者
                 * 一次）；回放成功路再与折叠成功路 MERGE 继续主序。 */
                ir_ref esucc = ir_END();
                ir_MERGE_N((ir_ref)j->mfail_n, j->mfail);
                j->mfail_n = 0;
                lower_replay(j, j->p_seq_pc, pc + 1);
                ir_ref ereplay = ir_END();
                ir_MERGE_2(ereplay, esucc);
            }
            pc++;
            break;
        }
        case OP_JMP: {
            uint32_t T = op->ar;
            if (j->loop_sp > 0 && T == j->loop_hdr[j->loop_sp - 1])
                return trap_lb();   /* patched by loop-condition handler */
            return trap_end(ir_END(), T);
        }
        case OP_JMPZ: case OP_JMPNZ: {
            uint32_t T = op->ar;
            if (T <= pc) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);

            /* loop condition: first JMPZ at loop top level */
            if (j->loop_sp > 0 && j->loop_cond[j->loop_sp - 1]) {
                j->loop_cond[j->loop_sp - 1] = 0;
                uint32_t E = T;
                ir_ref truthy = cond_truthy(j, pc, op);
                /* JMPZ: truthy → body; JMPNZ at header: reverse (rare) */
                ir_ref ifx;
                if (op->op == OP_JMPZ) ifx = ir_IF(truthy);
                else ifx = ir_IF(ir_NOT_B(truthy));
                ir_IF_TRUE(ifx);
                Trap tb = emit_seq(j, pc + 1, stops, nstops);
                if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0] == '2')
                    fprintf(stderr, "[jit] loopcond pc=%u T=%u sp=%d tail=%d\n",
                            pc, T, j->loop_sp, (int)tb.kind);
                if (tb.kind == TR_FAIL) return tb;
                if (tb.kind == TR_LOOPBACK) {
                    /* P0-14 批5: if 内 continue 的 pending 回边与体尾回跳
                     * 一次性 MERGE 后再 LOOP_END——LOOP_BEGIN 保持单回边
                    * pos2（多回边需 3+ 输入，零 deps/ir 改动的代价是统一
                     * 汇合点：控制流等价，PHI 无涉——本发射器无 SSA phi） */
                    int k = j->loop_sp - 1;
                    ir_ref le, bepay = 0;
                    if (j->lb_np[k] > 0) {
                        ir_ref in[JIT_LB_MAX + 1];
                        int n = j->lb_np[k];
                        for (int bi = 0; bi < n; bi++) in[bi] = j->lb_pend[k][bi];
                        in[n] = ir_END();          /* 体尾回跳区 */
                        j->lb_np[k] = 0;
                        ir_MERGE_N((ir_ref)(n + 1), in);
                        /* 批C slice-1：continue 路径时 reconcile phi 不支
                         * 配全部回边前驱 → 合并块内读槽（权威值，fast 线
                         * 已 store pay）兜底 */
                        if (j->acc_on && j->acc_phi)
                            bepay = load_pay(j, slot_addr(j, j->acc_slot));
                        le = ir_LOOP_END();
                    } else {
                        /* 批C slice-1：零 continue → reconcile phi 支配
                         * 回边，零 load 喂 phi；acc_iter 未构建（fold 关
                         * 断 / 快线未发射）→ 槽 load 兜底（权威值，fast
                         * 线每迭代 store pay；generic 路径槽恒 current） */
                        if (j->acc_on && j->acc_phi)
                            bepay = j->acc_iter ? j->acc_iter
                                : load_pay(j, slot_addr(j, j->acc_slot));
                        le = ir_LOOP_END();
                    }
                    ir_MERGE_SET_OP(j->loop_ref[k], 2, le);
                    if (j->acc_on && j->acc_phi)
                        ir_PHI_SET_OP(j->acc_phi, 2, bepay);
                } else {
                    do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);   /* body must back-edge or fail */
                }
                ir_IF_FALSE(ifx);
                /* exit path continues at E inside the false region */
                j->loop_sp--;
                j->acc_on = 0; j->acc_phi = j->acc_iter = 0; j->acc_slot = 0;
                pc = E;
                break;
            }

            uint32_t join, else_start;
            if (op->op == OP_JMPNZ && j->loop_sp > 0)
                j->lb_jmpnz[j->loop_sp - 1] = 1;   /* || 求值标记（挡板因子） */
            if (!find_join(j->fn, pc, T, &join, &else_start))
                do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);

            ir_ref truthy = cond_truthy(j, pc, op);
            int jmp_on_true = (op->op == OP_JMPNZ);
            /* P0-14 批5: ifx 恒 truthy。JMPNZ（||短路）原 NOT_B 极性反置
             * 且 tA=END@T 漏 emit 跳转目标体——该路径此前 100% fail 回退
             * （fail-safe 从未出错码），本次修正为对称两分支：
             * 真→emit(T..join) 跳转目标体；假→emit(pc+1..) fallthrough */
            ir_ref ifx = ir_IF(truthy);

            ir_IF_TRUE(ifx);
            Trap tA;
            if (jmp_on_true) {
                uint32_t st[11]; int ns = nstops + 1;
                if (nstops > 10) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = join;
                tA = emit_seq(j, T, st, ns);   /* 真跳目标体 → join */
                if (tA.kind == TR_FAIL) return tA;
                if (tA.kind == TR_LOOPBACK) {
                    if (!lb_push(j, ir_END())) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                    tA = trap_ret();
                }
                if (tA.kind == TR_END && tA.target != join)
                    do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
            } else {
                uint32_t st[11]; int ns = nstops + 1;
                if (nstops > 10) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = T;
                tA = emit_seq(j, pc + 1, st, ns);
                if (tA.kind == TR_FAIL) return tA;
                if (tA.kind == TR_LOOPBACK) {
                    /* P0-14 批5: then 内 continue——分支 END 存 pending，
                     * 按终端参与既有 RET 合流（ir_END 已闭合本区） */
                    if (!lb_push(j, ir_END())) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                    tA = trap_ret();
                }
                if (tA.kind == TR_END && tA.target != join &&
                    !(else_start && tA.target == else_start))
                    do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
            }

            ir_IF_FALSE(ifx);
            Trap tB;
            if (!jmp_on_true) {
                if (!else_start) {
                    tB = trap_end(ir_END(), T);
                } else {
                    uint32_t st[11]; int ns = nstops + 1;
                    if (nstops > 10) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                    if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                    st[nstops] = join;
                    tB = emit_seq(j, else_start, st, ns);
                    if (tB.kind == TR_FAIL) return tB;
                    if (tB.kind == TR_LOOPBACK) {
                        /* P0-14 批5: else 内 continue——同 tA 转终端 */
                        if (!lb_push(j, ir_END())) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                        tB = trap_ret();
                    }
                    if (tB.kind == TR_END && tB.target != join)
                        do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                }
            } else {
                /* P0-14 批5: JMPNZ 假侧 fallthrough → join（stops 含 T：
                 * 若中途落 T=else_start 则保守 END@T → 合流 fail 回退） */
                uint32_t st[12]; int ns = nstops + 2;
                if (nstops > 10) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = T;
                st[nstops + 1] = join;
                tB = emit_seq(j, pc + 1, st, ns);
                if (tB.kind == TR_FAIL) return tB;
                if (tB.kind == TR_LOOPBACK) {
                    if (!lb_push(j, ir_END())) do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
                    tB = trap_ret();
                }
                if (tB.kind == TR_END && tB.target != join &&
                    !(tB.target == T))
                    do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
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
            do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
        }
        default:
            do { if (!j->fail_line) j->fail_line = __LINE__; return trap_fail(); } while (0);
        }
    }
}

/* ================================================================
 * Compile entry
 * ================================================================ */
/* 批2-7: eager 编译延退（jit.h 原型注）——INIT_MCALL/NEW_USER 的 ic 由
 * 解释器执行该 op 时固化（init_mcall_core / op_new_user 自治填充），首调
 * 时 ic 必为 NULL → 折叠判不了只能回放；延退到固化后的下一次调用再编，
 * 折叠直调才真生效。NEW_USER ic==1（解析失败哨兵，每次重试）不延退：
 * 等一个可能永不到来的固化会拖掉小函数的急切编译（回放同样正确）。 */
bool tphp_jit_defer_ic(TFunc *fn) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if (op->op == OP_INIT_MCALL && !op->ic) return true;
        if (op->op == OP_NEW_USER && !op->ic) return true;
    }
    return false;
}

/* Compile all direct callees first so their jit_entry is set when this
 * function runs (direct-slot ABI: caller embeds TFunc* and looks up
 * jit_entry at runtime — NULL entry would read garbage ret). */
static void jit_compile_callees(TFunc *fn) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        /* 批3-3: INIT_SMCALL 的 ic 同为 TFunc*（eligibility 已解析缓存，
         * 无 builtin 标记位）——阈值逻辑与 INIT_FCALL 完全共用；
         * 批2-7: INIT_MCALL 的 ic 是 TMethod*（init_mcall_core 固化，
         * 需 sm->fn 解出 TFunc*），callee 连带编译让 emit 期 jit_entry
         * 就绪（否则函数级折叠退化回 p_seq=2 回放） */
        TFunc *callee;
        if (op->op == OP_INIT_FCALL || op->op == OP_INIT_SMCALL) {
            callee = (TFunc *)op->ic;
            if (!op->ic || ((uintptr_t)op->ic & 1)) continue;
        } else if (op->op == OP_INIT_MCALL) {
            TMethod *sm = (TMethod *)op->ic;
            if (!sm || !sm->fn) continue;
            callee = sm->fn;
        } else if (op->op == OP_NEW_USER) {
            /* 批2-8: userland ctor 连带编译——emit 期 entry 就绪
             * （否则 cfold 门落回放，折叠收益为空）。ic==1 解析失败哨兵/
             * 无 ctor/native 不扫。 */
            TClass *nc = (op->ic && op->ic != (void *)1) ? (TClass *)op->ic : NULL;
            if (!nc || nc->m_ctor < 0) continue;
            TMethod *cm = &nc->methods[nc->m_ctor];
            if (!cm->fn || cm->is_abstract || cm->native) continue;
            callee = cm->fn;
        } else continue;
        if (callee == fn) continue;           /* self-recursion: entry already set */
        /* 批5: 尊重急切/热阈值——绕过阈值连带编译把大 callee 的编译成本
         * 塞进 caller 首次调用的计时（logstat n_ops=106 被闭包连带编译
         * +85ms 实测）。阈值不满足→不编→下方 lower_call_finish 的
         * `!callee->jit_entry → fail` 让 caller 回退解释器（既有兜底）；
         * callee 被解释器调热(hot≥127)后自然编译，caller 随后可编。 */
        if (!callee->jit_entry && callee->hot != UINT16_MAX &&
            (callee->n_ops <= tphp_jit_eager_max_ops() ||
             callee->hot >= TPHP_JIT_HOT_THRESHOLD))
            tphp_jit_compile(callee);         /* recursive; lock is re-entry safe
                                               * via the jit_entry check below */
    }
}

/* ================================================================
 * 刀3（11-05）槽级标量写者分析 —— 回收刀2 释放门税
 * （memory/2026-11-04 刀2 结案"回收候选"；backlog 11-04 快照）
 * ================================================================
 * 查询侧 dst_gate_needed：槽 s 的全部写者皆产标量/UNDEF → 写点免发
 * rel_dst_gate。门税量化（刀2 A/B）：array-read +9.8% / fib +13.7% /
 * call +10.6% / kv −5.8%——门对标量恒空转，成本=动态装载比较+静态
 * 布局（刀1 教训：静态节点税独立于动态分支）。
 *
 * 分析 = 流不敏感全函数 fixpoint（hot 乐观起步 0、写者约束单调降级
 * 到 1，收敛 ≤ nslots 轮）：
 * - 无条件标量写者：tval_{add,sub,mul,div,mod,pow,band,bor,bxor,shl,
 *   shr,neg,pos,bit_not} 全部只回 int/float（或 raise——raise 不落写），
 *   BW_* 经 tval_to_int 亦回 int；cmp/bool 恒 bool/int；CAST ct∈{1,2,3}
 *   （函数级白名单保证）恒 int/bool/float；ISSET_* 恒 bool；UNSET_CV
 *   写 UNDEF。
 * - PROP（类传播）写者：COPY/ASSIGN/INC/DEC 结果类=源类——INC 的
 *   "标量入→标量出、串入→串出"（soundness：INC 可回写字符串，不可
 *   无条件白名单）；suffix 结果=旧值同类。INC 原位写 a1 为恒等传播，
 *   无需降级。
 * - 无条件 hot 写者：RECV（参数任意值，含 thunk/默认——OSR 面）/
 *   FETCH_DIM_R 元素/CONCAT 串/ARRAY_INIT·APPEND·ASSIGN_DIM 数组
 *   （autoviv/COW 写基槽 a1）/CLOSURE_INIT/NEW_USER 对象/FETCH_PROP
 *   属性任意/FE_RESET 数组·Generator/DO_FCALL 返回任意（ret_type
 *   收紧留后续批）/OP_CONST 非标量字面量（串）。
 * - 入口态预填：捕获 [0..n_cap) 与方法 slot0（$this/保留位由调用方
 *   直写帧，非 op 写）置 1；参数由 RECV 写者覆盖（JIT→JIT 直写同槽
 *   同域）；解释器帧 slots 全 UNDEF 起（vm.c frame_alloc）；tphp 无
 *   引用类型 → 零别名逃逸。
 * - 隐写点：FE_FETCH 的 val/key 在 slots[a2+1]/[a2+2] 不入操作数 →
 *   显式降级（漏标 = 覆盖串不释放 → 泄漏族）；a2 游标原位裸写 payload
 *   （CONST int 起恒 int）与 ar=bool 结果恒标量不降级。
 * - 读操作数绝不降级：JMPZ/JMPNZ 只读 cond、SEND 源、RETURN 值、
 *   INIT_* 状态——否则 cond TMP 被读侧污染，cmp 门白丢回收。
 * - **is_main 全槽保门**：global_slot 直接返回 main_frame->slots[i]
 *   （vm.c:329）——带 `global` 的函数（FETCH/ASSIGN_GLOBAL，JIT 白名单
 *   外=纯解释器）随时写主帧槽，操作数流分析对 main 不封闭。
 * - 未知 opcode（OSR chunk 可含函数级白名单外 op；ASSIGN_PROP/UNSET_PROP
 *   族经 replay 执行，其 handler 有 ar 写点）→ CV/TMP 操作数保守全降级
 *   （多保门 = HEAD 行为）。
 * 错判方向：该 hot 未 hot = 跳过释放 → 只可能泄漏（探针拦截），不会
 * 双释放/崩溃；反向错判 = 多付门 = HEAD。 */
static bool slotgate_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TPHP_SLOTGATE");
        on = (e && e[0] == '0') ? 0 : 1;
    }
    return on;
}

static void jit_scan_slot_writers(TFunc *fn, uint8_t *hot) {
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    if (!nslots) return;
    /* 源类：CONST 看字面量、槽看 hot、未知形态保守 hot */
#define SLOT_SRC_HOT(kind, idx) \
    ((kind) == OPD_CONST ? !lit_scalar(fn->lits[(idx)]) \
     : ((kind) == OPD_CV || (kind) == OPD_TMP) ? hot[(idx)] : 1)
#define SLOT_MARK(s) do { \
        uint32_t _sm = (uint32_t)(s); \
        if (_sm < nslots && !hot[_sm]) { hot[_sm] = 1; ch = 1; } \
    } while (0)

    /* 入口态预填：捕获槽由帧建立机制（vm.c frame push rec->clo 分支）
     * 预填、方法 slot0（$this）由调用方直入 poff 写——皆非本函数 op 写 */
    for (uint16_t i = 0; i < fn->n_cap && i < nslots; i++) hot[i] = 1;
    if (fn->is_method) hot[0] = 1;
    if (fn->is_main) {          /* 见函数注：全局别名随时写主帧槽 */
        memset(hot, 1, nslots);
        return;
    }

    for (uint32_t pass = 0; pass <= nslots + 1; pass++) {
        int ch = 0;
        for (uint32_t pc = 0; pc < fn->n_ops; pc++) {
            TOp *op = &fn->ops[pc];
            bool dsz = (op->tr == OPD_CV || op->tr == OPD_TMP);
            switch (op->op) {
            /* ---- 只读：绝不写槽（JMPZ/JMPNZ 只读 cond——soundness 要点） */
            case OP_JMP: case OP_JMPZ: case OP_JMPNZ: case OP_RETURN:
            case OP_SEND_VAL: case OP_SEND_VAR: case OP_SEND_CAP:
            case OP_INIT_FCALL: case OP_INIT_DYN: case OP_INIT_MCALL:
            case OP_INIT_SMCALL: case OP_INIT_NEW:
                break;
            /* ---- 无条件标量写者（dst=ar；a1/a2 只读） */
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_POW: case OP_BW_AND: case OP_BW_OR: case OP_BW_XOR:
            case OP_SHL: case OP_SHR: case OP_NEG: case OP_POS:
            case OP_BIT_NOT: case OP_SEQ: case OP_SNE: case OP_EQ:
            case OP_NE: case OP_CMP: case OP_LT: case OP_LE:
            case OP_BOOL_NOT: case OP_BOOL_VAL:
            case OP_ISSET_DIM: case OP_ISSET_PROP: case OP_ISSET_CV:
                break;
            case OP_UNSET_CV:
                break;              /* 写 UNDEF（dst=a1） */
            case OP_CAST:
                if (op->t2 == OPD_CONST) {
                    int64_t ct = fn->lits[op->a2].u.i;
                    if (ct == 1 || ct == 2 || ct == 3) break;  /* int/bool/float */
                }
                if (dsz) SLOT_MARK(op->ar);   /* 未知 ct（串转型等）→ 串 */
                break;
            /* ---- PROP：结果类 = 源类（COPY 只标 dst） */
            case OP_COPY: case OP_ASSIGN:
            case OP_INC: case OP_DEC:
                if (dsz && SLOT_SRC_HOT(op->t1, op->a1)) SLOT_MARK(op->ar);
                break;
            /* ---- 字面量：非标量（串）→ hot */
            case OP_CONST:
                if (dsz && !lit_scalar(fn->lits[op->a1])) SLOT_MARK(op->ar);
                break;
            /* ---- 无条件 hot 写者 */
            case OP_RECV:            if (dsz) SLOT_MARK(op->ar); break;  /* 参数任意值 */
            case OP_ARRAY_INIT:      if (dsz) SLOT_MARK(op->ar); break;  /* 数组 */
            case OP_FETCH_DIM_R:     if (dsz) SLOT_MARK(op->ar); break;  /* 元素任意 */
            case OP_CONCAT:          if (dsz) SLOT_MARK(op->ar); break;  /* 串 */
            case OP_CLOSURE_INIT:    if (dsz) SLOT_MARK(op->ar); break;  /* T_CLOSURE */
            case OP_NEW_USER:        if (dsz) SLOT_MARK(op->ar); break;  /* 对象 */
            case OP_FETCH_PROP:      if (dsz) SLOT_MARK(op->ar); break;  /* 属性任意 */
            case OP_FE_RESET:        if (dsz) SLOT_MARK(op->ar); break;  /* 数组/Generator */
            case OP_DO_FCALL:        if (dsz) SLOT_MARK(op->ar); break;  /* 返回任意 */
            case OP_ARRAY_APPEND:
            case OP_ASSIGN_DIM:
                /* 基槽 a1 可能 autoviv/COW 成数组；ar/t2 = val/key 只读
                 *（do_dim_write 只写 bp，rdv 取 val） */
                if (op->t1 == OPD_CV || op->t1 == OPD_TMP) SLOT_MARK(op->a1);
                break;
            case OP_FE_FETCH:
                /* 隐写 val/key 在 slots[a2+1]/[a2+2]（vm.c op_fe_fetch，
                 * 不入操作数）→ 显式降级；ar=bool 结果、a2 游标恒标量 */
                SLOT_MARK(op->a2 + 1);
                SLOT_MARK(op->a2 + 2);
                break;
            default:
                /* 未知 op（OSR 白名单外 / replay 族）：CV/TMP 操作数保守降级 */
                if (op->t1 == OPD_CV || op->t1 == OPD_TMP) SLOT_MARK(op->a1);
                if (op->t2 == OPD_CV || op->t2 == OPD_TMP) SLOT_MARK(op->a2);
                if (dsz) SLOT_MARK(op->ar);
                break;
            }
        }
        if (!ch) break;
    }
#undef SLOT_MARK
#undef SLOT_SRC_HOT
}

void tphp_jit_compile(TFunc *fn) {
    if (!g_jit_on || !fn) return;
    if (fn->jit_entry) return;
    /* 刀18 诊断：编译成本计时（含 compile_callees 连带；DEBUG 门）——
     * 急切门扩容 A/B 需要把"成本 vs 收益"拆开判（刀6 教训） */
    uint64_t ct0 = jit_dbg() ? uv_hrtime() : 0;
    if (!tphp_jit_eligible(fn)) {
        fn->hot = UINT16_MAX;
        return;
    }
    /* ensure callees compiled first (before we take the lock — tphp_jit_compile
     * re-locks; the jit_entry early-return makes it idempotent) */
    jit_compile_callees(fn);
    jit_lock();
    if (fn->jit_entry) { jit_unlock(); return; }

    JitC j;
    memset(&j, 0, sizeof j);
    j.fn = fn;
    /* 刀3：槽级写者分析（TPHP_SLOTGATE=0 → slot_hot=NULL → 全保门） */
    if (slotgate_on() && fn->n_locals + fn->n_tmps) {
        j.slot_hot = (uint8_t *)calloc((size_t)fn->n_locals + fn->n_tmps, 1);
        if (j.slot_hot) jit_scan_slot_writers(fn, j.slot_hot);
    }
    ir_ctx *ctx = tm_malloc(sizeof(ir_ctx));
    memset(ctx, 0, sizeof *ctx);
    j.ctx = ctx;
    ir_init(ctx, IR_FUNCTION | IR_OPT_FOLDING
                 | ((getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0] == '2')
                    ? (1u << 30) : 0),   /* IR_DEBUG_RA 位（deps/ir 需 -DIR_DEBUG 才出日志） */
            IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
    ctx->ret_type = IR_I32;

    /* IR requires START before PARAMs (see deps/ir/examples 0001/0008).
     * v1.2 direct-slot ABI: params already in slots[0..n_params-1] (INT
     * guaranteed by caller for JIT→JIT; interpreter path guards in C).
     * No prologue guard/copy — that was the fib call-overhead hot spot. */
    ir_START();
    j.r_vm = ir_PARAM(IR_ADDR, "vm", 1);
    j.r_slots = ir_PARAM(IR_ADDR, "slots", 2);
    j.r_ret = ir_PARAM(IR_ADDR, "ret", 3);
    /* P0-15 B 的未初始化槽保护（10-30 CI 定位，当前态=S1 无自填）：
     * 原 tmps-only 自填被 O1 跨 FE replay call 折叠成循环 load=UNDEF/NULL
     *（fe_kv_sum $v=NULL gdb 实锤，删即绿）；改填 CV 区 [n_params..n_locals)
     * 又因方法 poff=1 布局起点错覆盖参数（18 OOP 失败）。caller 填满 64
     * 上界则 126 store/调用吃光 P0-15 收益（kv 39→53）。三案全撤。
     * 残留理论风险：B FAST r_call_slots 的非参数 CV 未初始化读语义
     *（无测试暴露，出现即修——正解=IR 动态循环填 [argc..nlocals)，需 phi
     * 结构，与嵌套 loop 雷区同域慎入）。 */

    /* entry-block call scratch: one ALLOCA per function, reused by every
     * call site (loop-body ALLOCA stacks up under O1 → overflow ~5k iters) */
    j.r_call_args = ir_ALLOCA(ir_CONST_I64(9 * TVAL_SZ));
    j.r_call_clo  = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
    j.r_call_ret  = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
    j.r_call_slots = ir_ALLOCA(ir_CONST_I64(64 * TVAL_SZ));

    uint32_t pc0 = 0;
    while (pc0 < fn->n_ops && fn->ops[pc0].op == OP_RECV) pc0++;

    Trap t = emit_seq(&j, pc0, NULL, 0);
    int ok = (!j.fail && t.kind == TR_RET);

    void *entry = NULL;
    if (ok) {
        size_t size = 0;
        if (jit_dbg()) {
            fprintf(stderr, "[jit] ir_check...\n"); fflush(stderr);
            bool chk = ir_check(ctx);
            fprintf(stderr, "[jit] ir_check=%d\n", chk); fflush(stderr);
            /* dump IR graph for crash diagnosis (TPHP_JIT_DEBUG=2) */
            if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0] == '2') {
                fprintf(stderr, "[jit] ir dump:\n");
                ir_dump(ctx, stderr);
                fflush(stderr);
                /* P0-17③ 侦察：生成码（phi 链 spill/寄存器驻留实况） */
                fprintf(stderr, "[jit] codegen dump:\n");
                ir_dump_codegen(ctx, stderr);
                fflush(stderr);
            }
        }
        /* O1: skip SCCP. O2/SCCP has a multi-slot CALL bug (deps/ir
         * ir_sccp_transform treats tail slots of 7-arg CALL as real insns
         * and zeros an operand → codegen crash on chain TMP-ADD). See
         * tools/ir_bisect.c repro; restore O2 after upstream fix. */
        entry = ir_jit_compile(ctx, 1, &size);   /* O1 */
        /* P0-17③ 侦察：落盘机器码供外部反汇编（DEBUG=2；TEMP 覆盖式） */
        if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0] == '2' && entry) {
            const char *tmp = getenv("TEMP");
            char path[512];
            snprintf(path, sizeof path, "%s\\tp_jitcode.bin", tmp ? tmp : ".");
            FILE *bf = fopen(path, "wb");
            if (bf) { fwrite(entry, 1, size, bf); fclose(bf); }
        }
        if (jit_dbg())
            fprintf(stderr, "[jit] compile(%s) entry=%p size=%zu\n",
                    fn->name ? tstr_val(fn->name) : "?", entry, size);
    } else {
        if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0]) {
            uint32_t pc = j.cur_pc < fn->n_ops ? j.cur_pc : 0;
            fprintf(stderr,
                    "[jit] compile(%s) FAILED emit (trap=%d pc=%u op=%u %s L%d) at %s:%u n_ops=%u\n",
                    fn->name ? tstr_val(fn->name) : "?", (int)t.kind, j.cur_pc, (unsigned)fn->ops[pc].op, tphp_op_name(fn->ops[pc].op), j.fail_line,
                    fn->file ? fn->file : "?", (unsigned)fn->ops[0].line, fn->n_ops);
        }
    }
    ir_free(ctx);
    tm_free(ctx);
    free((void *)j.slot_hot);

    if (entry)
        fn->jit_entry = entry;
    else
        fn->hot = UINT16_MAX;
    if (jit_dbg())
        fprintf(stderr, "[jit] compile_cost(%s) %.2f ms\n",
                fn->name ? tstr_val(fn->name) : "?", (uv_hrtime() - ct0) / 1e6);
    jit_unlock();
}

/* ================================================================
 * 循环 OSR（P1-12 批1，PLAN/osr.md）
 * ================================================================ */
/* 批1 白名单：封闭 int/float 域内纯计算 + 控制流。循环体子图内排除
 * CALL/DIM/ARRAY/RETURN/FE/EXTRACT 等一切带 raise 或跨帧语义的 op（批2
 * 经 OSR_RET_PC 的 deopt=resume_pc 语义再扩）。emit 期仍可能 fail 的
 * 形态（如变量移位量）在 OSR 模式下安全退化为"该 op 回解释器重跑"
 * （OSR_RET_PC），无需在此复刻函数级的形态限制。 */
static bool osr_body_ok(TFunc *fn, uint32_t header, uint32_t backedge) {
    for (uint32_t i = header; i <= backedge && i < fn->n_ops; i++) {
        switch (fn->ops[i].op) {
        case OP_CONST: case OP_COPY: case OP_ASSIGN:
        case OP_ADD: case OP_SUB: case OP_MUL:
        case OP_NEG: case OP_POS: case OP_INC: case OP_DEC:
        case OP_BW_AND: case OP_BW_OR: case OP_BW_XOR: case OP_BIT_NOT:
        case OP_SHL: case OP_SHR:
        case OP_BOOL_NOT: case OP_BOOL_VAL: case OP_CAST:
        case OP_EQ: case OP_NE: case OP_SEQ: case OP_SNE:
        case OP_LT: case OP_LE: case OP_CMP:
        case OP_ISSET_CV: case OP_UNSET_CV:
        case OP_JMP: case OP_JMPZ: case OP_JMPNZ:
            break;
        case OP_FETCH_DIM_R:
            /* P1-12 批2-1（PLAN/osr.md §7）：读操作幂等——deopt 重跑安全
             * （IC miss → helper 返 0 → OSR_RET_PC=cur_pc → 解释器重跑
             * op_fetch_dim_r；helper 内 raise_guarded 不穿透，异常由解释
             * 器重跑时抛出，无重复副作用）。形态守卫对齐函数级
             * eligibility（jit.c:669 无键形态拒）。emit 复用 emit_seq 的
             * 函数级 case（packed 直取 + DimIC 链全内联），IC 懒分配在
             * emit 期单次（触发线程即执行线程，同步编译无并发执行）。 */
            if (fn->ops[i].t2 == OPD_UNUSED) return false;
            break;
        case OP_FETCH_PROP: case OP_ASSIGN_PROP:
        case OP_ISSET_PROP: case OP_UNSET_PROP:
            /* P1-12 批2-2a：单 op pad 执行（lower_replay→tphp_jit_opseq）。
             * opseq 返 0 恒=exception（vm.c setjmp 唯一出口，prop handler
             * 错误全走 raise）→ 异常传播不重跑，deopt resume=cur_pc 恒
             * 安全；成功路径 handler 已写 dst 槽（真 handler 语义零漂移）。
             * magic __get/__set 的用户码 raise 同门。 */
            break;
        case OP_INIT_FCALL: {
            /* P1-12 批2-2b：折叠 CALL 序放行。用户 callee 准入对齐函数级
             * （jit_eligible_inner 递归）；builtin（ic|1）bcall fail=
             * exception。deopt 面：直调 guard fail（entry NULL/INT 契约/
             * callee deopt 未执行）→ OSR_SEQ_PC=序起点重跑整序幂等
             * （SEND 源恒槽、args 表达式在 seq 外）。 */
            TFunc *callee = (TFunc *)fn->ops[i].ic;
            if (!callee) return false;
            if (!((uintptr_t)callee & 1) && callee != fn &&
                !jit_eligible_inner(callee, 1))
                return false;
            break;
        }
        case OP_SEND_VAL: case OP_SEND_VAR:
            if (fn->ops[i].t1 == OPD_CONST &&
                !lit_scalar(fn->lits[fn->ops[i].a1]) &&
                fn->lits[fn->ops[i].a1].t != T_STR)
                return false;
            break;
        case OP_DO_FCALL: case OP_INIT_DYN:
            /* 动态调用折叠走 opseq 深跑（fail=exception）或直调链
             * （fail→OSR_SEQ_PC）；DO/INIT_DYN 自身无形态约束。 */
            break;
        case OP_INIT_MCALL: case OP_INIT_NEW: case OP_NEW_USER:
            /* P1-12 批2-3 重验（2026-11-02，f97555a 后无混杂会话）：对象/
             * 方法链 opseq replay 同构（fail=exception 论证同批2-2a）。10-31
             * 首次尝试撤回 = O1+FOLDING 在 LOOP×opseq-replay-MERGE 上 CSE
             * 产坏码（tv_copy src=0，deps/ir 域）——kv 线同款"卡上游"归因
             * 已被 f97555a 证伪，本会话实测该阻塞是否同因：绿则落地，复现
             * 则再撤并转最小复现报 dstogov（PLAN/osr.md 批2-3 三态矩阵）。 */
            break;
        default:
            if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0])
                fprintf(stderr, "[jit] osr reject: pc=%u op=%s\n",
                        i, tphp_op_name(fn->ops[i].op));
            return false;
        }
    }
    return true;
}

/* 收集循环出口（> backedge 的正向跳转 target + 体尾 fall-through），
 * 全部作 emit_seq 的 stops——循环后的代码留给解释器（resume）。 */
static int osr_collect_exits(TFunc *fn, uint32_t header, uint32_t backedge,
                             uint32_t *exits, int cap) {
    int ne = 0;
    if (backedge + 1 < fn->n_ops) exits[ne++] = backedge + 1;
    for (uint32_t i = header; i <= backedge && i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if ((op->op == OP_JMP || op->op == OP_JMPZ || op->op == OP_JMPNZ) &&
            op->ar > backedge && op->ar < fn->n_ops) {
            int seen = 0;
            for (int k = 0; k < ne; k++) if (exits[k] == op->ar) seen = 1;
            if (!seen) {
                if (ne >= cap) return -1;      /* 出口过多：保守拒编 */
                exits[ne++] = op->ar;
            }
        }
    }
    return ne;
}

void *tphp_jit_compile_osr(TFunc *fn, uint32_t header, uint32_t backedge,
                           uint32_t backedge_cur) {
    if (!g_jit_on || !fn) return NULL;
    if (header >= backedge || backedge >= fn->n_ops) return NULL;
    /* main 允许（OSR 不走函数入口 ABI，slots 直用——顶层循环正是批1 的
     * 核心场景）；is_global 别名槽排除（全局变量读写语义不在封闭子集）。
     * closure 允许（n_cap>0 拒）；variadic/generator/异常表拒。 */
    if (fn->is_variadic || fn->is_generator || fn->n_ex > 0 || fn->n_cap > 0)
        return NULL;
    if (fn->is_global) {
        for (uint16_t i = 0; i < fn->n_locals; i++)
            if (fn->is_global[i]) return NULL;
    }
    if (fn->param_types) {
        for (uint16_t i = 0; i < fn->n_params; i++)
            if (fn->param_types[i]) return NULL;
    }
    if (!jit_cfg_ok(fn)) return NULL;
    if (!osr_body_ok(fn, header, backedge)) return NULL;

    uint32_t exits[16];
    int ne = osr_collect_exits(fn, header, backedge, exits, 16);
    if (ne <= 0) return NULL;

    jit_lock();

    JitC j;
    memset(&j, 0, sizeof j);
    j.fn = fn;
    j.osr = 1;
    j.cur_pc = backedge_cur;
    /* 刀3：槽级写者分析（同一 fn 全流分析，OSR chunk 写点同样受约束） */
    if (slotgate_on() && fn->n_locals + fn->n_tmps) {
        j.slot_hot = (uint8_t *)calloc((size_t)fn->n_locals + fn->n_tmps, 1);
        if (j.slot_hot) jit_scan_slot_writers(fn, j.slot_hot);
    }
    ir_ctx *ctx = tm_malloc(sizeof(ir_ctx));
    memset(ctx, 0, sizeof *ctx);
    j.ctx = ctx;
    /* FOLDING 保留（批2-3 重验 2026-11-02）：10-31 记录的 O1+FOLDING CSE
     * 坏码（LOOP×opseq-replay-MERGE，tv_copy src=0）在 f97555a 后的本会话
     * 不复现——test-osr-005（方法/new+prop+COPY/组合三形态）10 连跑全绿、
     * osr ir_check=1、阈值关断对照证实 chunk 真执行（4349→3755ms）。
     * MCALL/NEW 已进 osr_body_ok 白名单（PLAN/osr.md 批2-3 节）。 */
    ir_init(ctx, IR_FUNCTION | IR_OPT_FOLDING
                 | ((getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0] == '2')
                    ? (1u << 30) : 0),   /* IR_DEBUG_RA 位（deps/ir 需 -DIR_DEBUG 才出日志） */
            IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
    ctx->ret_type = IR_I32;

    ir_START();
    j.r_vm = ir_PARAM(IR_ADDR, "vm", 1);
    /* slots = 解释器当前帧槽区（VMFrame 与 JIT 布局一致，PLAN/osr.md §2
     * 的零拷贝红利）：OSR entry 直读直写解释器 live 状态，退出后解释器
     * 从 resume pc 继续，无任何状态转换。 */
    j.r_slots = ir_PARAM(IR_ADDR, "slots", 2);
    /* call scratch 与函数级同构分配（批1 白名单无 CALL，分配仅为防
     * lower_* 路径 NULL 解引用——批2 接入时零改动） */
    j.r_call_args = ir_ALLOCA(ir_CONST_I64(9 * TVAL_SZ));
    j.r_call_clo  = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
    j.r_call_ret  = ir_ALLOCA(ir_CONST_I64(TVAL_SZ));
    /* P1-12 批2-2：B 直调链读 j->r_call_slots——漏分配则 ref 0 断链
     * （ir_check=0 照常出码 → 崩；2026-10-31 c4 实测） */
    j.r_call_slots = ir_ALLOCA(ir_CONST_I64(64 * TVAL_SZ));

    Trap t = emit_seq(&j, header, exits, ne);
    /* TR_END = 循环出口（stop/fall-through/break 跳出）；TR_RET 不可能
     * （白名单无 RETURN，出现即白名单漏洞 → 拒编保守回退）；TR_LOOPBACK
     * 顶层冒泡 = 死循环形态（while(1) 无条件回边）→ 拒编。 */
    int ok = (!j.fail && t.kind == TR_END);
    if (ok) {
        /* 终端 END 块收尾：TR_END 仅 OSR 出现（函数级 stops=NULL，只可能
         * TR_RET/FAIL/LOOPBACK），此前出口块无 terminator，codegen 落出
         * 代码缓冲（实测 = 垃圾 resume_pc → VM 跳飞挂死，ir_check 不查
         * 尾块 terminator）。resume_pc = 出口 op 的 pc，解释器无缝续跑。 */
        ir_BEGIN(t.end);
        ir_RETURN(ir_CONST_I32((int32_t)t.target));
    }

    void *entry = NULL;
    if (ok) {
        size_t size = 0;
        if (jit_dbg()) {
            fprintf(stderr, "[jit] osr ir_check...\n"); fflush(stderr);
            bool chk = ir_check(ctx);
            fprintf(stderr, "[jit] osr ir_check=%d\n", chk); fflush(stderr);
            if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0] == '2') {
                fprintf(stderr, "[jit] osr ir dump:\n");
                ir_dump(ctx, stderr);
                fflush(stderr);
            }
        }
        entry = ir_jit_compile(ctx, 1, &size);   /* O1，同函数级 */
        if (jit_dbg())
            fprintf(stderr, "[jit] osr compile(%s) header=%u entry=%p size=%zu\n",
                    fn->name ? tstr_val(fn->name) : "?", header, entry, size);
    } else if (jit_dbg()) {
        uint32_t pc = j.cur_pc < fn->n_ops ? j.cur_pc : 0;
        fprintf(stderr,
                "[jit] osr compile(%s) FAILED emit (trap=%d pc=%u op=%u %s L%d)\n",
                fn->name ? tstr_val(fn->name) : "?", (int)t.kind, j.cur_pc,
                (unsigned)fn->ops[pc].op, tphp_op_name(fn->ops[pc].op),
                j.fail_line);
    }
    ir_free(ctx);
    tm_free(ctx);
    free((void *)j.slot_hot);

    jit_unlock();
    return entry;          /* NULL = 拒编/发射失败（调用方熔断该位点） */
}

/* 解释器 op_jmp back-edge 的统一入口：计数 → 达阈值编译 → 进 OSR。
 * slots 由调用方（vm.c）传 frame->slots——VM 对 jit.c 不透明。
 *
 * 状态编码 per-site 存 back-edge op 的 ic（JMP 类 op 无 DimIC 占用）：
 *   0..THRESHOLD-1  = back-edge 计数器
 *   bit63 tag       = 该位点的 OSR chunk 入口（JIT 代码分配在用户态
 *                     低位区，bit63 恒 0，可作 tag）
 *   UINTPTR_MAX     = 熔断（拒编/发射失败，该循环永不再试）
 *
 * chunk 必须循环级 per-site：曾挂函数级入口复用（osr_entry on TFunc），
 * 同函数第二个热循环直进第一个循环的 chunk——读的是别的循环的槽，
 * cond 常假 → 立即返第一个循环的出口 pc → 两循环间 ping-pong
 * （实测 4000 万圈，值全对但永远走不到第二个循环）。 */
#define OSR_IC_TAG (((uintptr_t)1) << 63)

bool tphp_osr_backedge(VM *vm, TFunc *fn, void **ic_slot, uint32_t header,
                       uint32_t backedge_cur, TVal *slots,
                       uint32_t *resume_pc) {
    typedef int (*OsrEntry)(VM *, TVal *);
    uintptr_t v = (uintptr_t)*ic_slot;
    if (v == UINTPTR_MAX) return false;        /* 熔断位点（先于 tag 判定） */
    if (v & OSR_IC_TAG) {                      /* 已挂：直进（免计数） */
        *resume_pc = (uint32_t)((OsrEntry)(void *)(v & ~OSR_IC_TAG))(vm, slots);
        return true;
    }
    if (!tphp_jit_available()) return false;
    /* 非原子竞态最坏计数漂移（少/多触发一次），同 P0-13 推理。 */
    v++;
    if (v < (uintptr_t)TPHP_JIT_OSR_THRESHOLD) {
        *ic_slot = (void *)v;
        return false;
    }
    void *entry = tphp_jit_compile_osr(fn, header, backedge_cur, backedge_cur);
    if (!entry) {
        *ic_slot = (void *)UINTPTR_MAX;        /* 熔断：该循环不再重试 */
        return false;
    }
    *ic_slot = (void *)((uintptr_t)entry | OSR_IC_TAG);
    *resume_pc = (uint32_t)((OsrEntry)entry)(vm, slots);
    return true;
}

#else /* !TPHP_JIT */

bool tphp_jit_available(void) { return false; }
void tphp_jit_init(void) {}
void tphp_jit_compile(TFunc *fn) { fn->hot = UINT16_MAX; }

#endif

