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
#include "jit.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void tphp_vm_frame_destroy(VMFrame *f);   /* vm.c */
TVal tphp_vm_call_callable(VM *vm, TVal callable, TVal *args, int n); /* vm.c */
TClosure *tphp_tclo_alloc0(TFunc *fn);                               /* vm.c */
int tphp_jit_opseq(VM *vm, uint32_t pc, uint32_t pc_end, TVal *recv_sc); /* vm.c */

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
    if (setjmp(mine) == 0) {
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
    if (setjmp(mine) == 0) {
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
    if (setjmp(mine) == 0) {
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
        if (setjmp(mine) == 0) {
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

/* CLOSURE_INIT 无捕获形态：镜像 op_closure_init（n_cap=0 时不碰 caps 栈） */
void tphp_jit_closure_init(VM *vm, TVal *dst, TFunc *self, uint32_t fn_idx) {
    Program *prog = self->prog ? self->prog : vm->prog;
    if (!prog || fn_idx >= (uint32_t)prog->n_funcs)
        tphp_vm_raise(vm, "Error", "Internal: closure function missing");
    TClosure *c = tphp_tclo_alloc0(prog->funcs[fn_idx]);
    tval_release(dst);
    *dst = TV_CLO(c);
}

/* INIT_DYN..DO_FCALL 动态调用（$f(...)）：JIT'd 无捕获闭包走 trampoline，
 * 其余回落 tphp_vm_call_callable（语义统一入口）。
 * 返回 0 = callee deopt（exception 已设）→ IR 立即 RETURN 0 沿链传播；
 * 不可落回解释器重跑（双重副作用 + exception 悬挂）。
 * tphp_jit_call 返 0 但无 exception（!entry / g_jsp 溢出，未执行）→
 * 仍走解释器 fallback。 */
int tphp_jit_call_dyn(VM *vm, TVal *clo_slot, TVal *args_in, int n, TVal *ret) {
    TVal clo = *clo_slot;
    if (clo.t == T_CLOSURE) {
        TClosure *c = (TClosure *)clo.u.p;
        /* P0-15 C：合并 call_dyn→tphp_jit_call 两层 C 帧；entry 查表提前；
         * 补 n==nparams 防越界（原版直接拷 nparams 个，n<nparams 读越界）。 */
        if (c->fn && !c->bfn && c->n_cap == 0) {
            JitEntry e = (JitEntry)c->fn->jit_entry;
            if (e) {
                uint32_t nparams = c->fn->n_params;
                uint32_t nslots = (uint32_t)c->fn->n_locals + c->fn->n_tmps;
                enum { JSIZE = 2048 };
                static _Thread_local TVal g_jstack[JSIZE];
                static _Thread_local uint32_t g_jsp;
                if (nparams == (uint32_t)n && g_jsp + nslots <= JSIZE) {
                    TVal *slots = &g_jstack[g_jsp];
                    g_jsp += nslots;
                    for (uint32_t i = 0; i < nparams; i++)
                        slots[i] = args_in[i];
                    for (uint32_t i = nparams; i < nslots; i++)
                        slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
                    int ok = e(vm, slots, ret);
                    g_jsp -= nslots;
                    if (ok) return 1;
                    if (vm->x.exception)
                        return 0;
                }
            }
        }
    }
    *ret = tphp_vm_call_callable(vm, clo, args_in, n);
    return 1;
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
    if (setjmp(mine) == 0) {
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

static bool jit_eligible_inner(TFunc *fn, int depth) {
    if (depth > 16) return false;
    if (!fn || fn->is_main ||
        fn->is_variadic || fn->is_generator || fn->n_ex > 0 ||
        fn->n_cap > 0 || fn->n_params > 8)
        return false;   /* is_closure 允许（v1.1）：n_cap>0 已在上一行拒绝。
                         * is_method 批2-2 解禁：$this@slot0/参数 1.. 布局由
                         * op_do_fcall 直入 poff 承接（vm.c）——方法进 JIT
                         * 唯一入口就是该直入（INIT_FCALL 不解析方法，JIT→JIT
                         * 到不了方法体），旧禁令=该布局债的另一半 */
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
        case OP_CLOSURE_INIT:
            if (op->a2 != 0) { rej = "CLOSURE_INIT captured"; goto out; }
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
        fprintf(stderr, "[jit] eligible(%s)=%d\n",
                fn->name ? tstr_val(fn->name) : "?", ok);
    return ok;
}

/* ================================================================
 * Lowering
 * ================================================================ */
typedef struct {
    ir_ctx *ctx;
    TFunc *fn;
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
    /* P0-14 批2: 调用序回放（INIT_NEW…NEW_USER / INIT_MCALL…DO_FCALL）。
     * 1 = new 序，2 = mcall 序，值即 p_seq_pc 起点 op 下标 */
    int p_seq;
    uint32_t p_seq_pc;
    /* pre-allocated call scratch (entry-block ALLOCA — loop-body ALLOCA
     * stacks up under O1 and overflows after ~5k iterations) */
    ir_ref r_call_args;         /* argv buffer, 9 * TVAL_SZ */
    ir_ref r_call_slots;        /* P0-15 B dyn direct-call slots, 64 * TVAL_SZ */
    ir_ref r_call_clo;          /* closure TVal scratch */
    ir_ref r_call_ret;          /* return TVal scratch */
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

/* P0-12：slow 调用带 deopt 分支（raise → helper 返 0 → IR RETURN 0 传播）。
 * 分支留开：调用方后续 END/续接自然落在 IF_FALSE 内（bcall 同款结构）。 */
static void call_slow_binop(JitC *j, int opi, ir_ref da,
                            ir_ref ta, ir_ref pa, ir_ref tb, ir_ref pb) {
    JIT_CTX();
    ir_ref args[7] = { j->r_vm, ir_CONST_I32(opi), da, ta, pa, tb, pb };
    ir_ref ok = ir_CALL_N(IR_I32, ir_CONST_ADDR((uintptr_t)tphp_jit_binop_slow), 7, args);
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_RETURN(ir_CONST_I32(0));
    ir_IF_FALSE(ifd);
}
static void call_slow_cmp(JitC *j, int opi, ir_ref da,
                          ir_ref ta, ir_ref pa, ir_ref tb, ir_ref pb) {
    JIT_CTX();
    ir_ref args[7] = { j->r_vm, ir_CONST_I32(opi), da, ta, pa, tb, pb };
    ir_ref ok = ir_CALL_N(IR_I32, ir_CONST_ADDR((uintptr_t)tphp_jit_cmp_slow), 7, args);
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_RETURN(ir_CONST_I32(0));
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
    return ir_NOT_B(falsy2);
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
} DimIcCtx;

/* miss：整链落 v1 helper + deopt（线留开供 MERGE；RETURN 线不参与）。
 * count=true = 进过 IC 链的失败——misses++（generic 回退门计数，锁死
 * 态与非数组/packed 越界形态不计）。 */
static void dimic_miss(DimIcCtx *g, bool count) {
    JitC *j = g->j;
    JIT_CTX();
    if (count) {
        ir_ref ma = ir_ADD_A(g->ic, ir_CONST_ADDR(offsetof(DimIC, misses)));
        ir_STORE(ma, ir_ADD_I32(ir_LOAD_I32(ma), ir_CONST_I32(1)));
    }
    ir_ref t[5] = { g->j->r_vm, g->dst, g->base, g->key, g->ic };
    ir_ref ok = ir_CALL_N(IR_I32,
                          ir_CONST_ADDR((uintptr_t)tphp_jit_fetch_dim_r), 5, t);
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_RETURN(ir_CONST_I32(0));
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
static void dimic_take(DimIcCtx *g, ir_ref vaddr) {
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

/* v3 多态链：lvl 0..2 = 共享层（arr/buckets/bpending，失败 = 整链 miss
 * 计数）；lvl 3..10 = cases[0..7] 按序（哨兵恒非 NULL → 每 case 归一单
 * bool，T=直接叶子取值、F=下一 case；第 8 case F 才 miss 计数）。结构
 * 仍是逐层 IF+END+MERGE_2——T/F 只有一个递归分支，线性无膨胀。 */
static ir_ref dimic_ic_chain(DimIcCtx *g, int lvl) {
    JitC *j = g->j;
    JIT_CTX();
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
        int ci = lvl - 3;
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
    bool last = (lvl == 3 + DIM_IC_CASES - 1);
    ir_ref ifx = ir_IF(cond);
    ir_IF_TRUE(ifx);
        ir_ref ct;
        if (leaf_here) {
            ir_ref voff = ir_LOAD_U32(
                ir_ADD_A(bp, ir_CONST_ADDR(offsetof(ABucket, val_off))));
            ir_ref data = ir_LOAD_A(ir_ADD_A(g->a, ir_CONST_ADDR(offsetof(TArr, data))));
            dimic_take(g, ir_ADD_A(data,
                         ir_SHL_U64(ir_ZEXT(IR_U64, voff), ir_CONST_U64(4))));
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
    return ir_END();
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

/* 无分支 bool 存储：tag = T_FALSE + SEXT(cond)（FALSE=1, TRUE=2）。
 * 钻石 MERGE 嵌在 LOOP 区域会触发 IR codegen 崩溃（ir_check 都拦不住），
 * 值选择一律走位运算；控制流 IF 只留给 JMPZ。 */
/* 无分支 mask：cond BOOL → i64 0/-1；i32 版 0/1 */
static void lower_un(JitC *j, TOp *op) {
    JIT_CTX();
    ir_ref aa = slot_addr(j, op->a1), da = slot_addr(j, op->ar);
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    switch (op->op) {
    case OP_COPY: case OP_ASSIGN:
        /* P0-1：裸 store 对数组/串双重释放 → 走 rc helper（标量 no-op） */
        {
            ir_ref t[2] = { da, aa };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_tv_copy), 2, t);
        }
        break;
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
    if (op->t2 != OPD_UNUSED) { j->fail = 1; do { j->fail_line = __LINE__; return; } while (0); }  /* dim 形态 $a[$i]++ 走解释器（本 lower 只镜像 var-RMW） */
    ir_ref aa = slot_addr(j, op->a1), da = aa;
    bool has_dst = (op->tr != OPD_UNUSED) &&
                   !dst_slot_unused(j->fn, pc, op->tr, op->ar);
    bool prefix = op->ext != 0;
    ir_ref dstA = has_dst ? slot_addr(j, op->ar) : aa;
    ir_ref ta = load_tag(j, aa), pa = load_pay(j, aa);
    if (has_dst && !prefix) {
        /* suffix（res=old）：src 未更新，标量旧值直存；非标量/UNDEF →
         * helper（UNDEF→NULL + addref，与 do_incdec 逐位同语义） */
        ir_ref ifs = ir_IF(ir_LT(ta, ir_CONST_I32(5)));   /* T_STR=5 起非标量 */
        ir_IF_TRUE(ifs);
            store_pay(j, dstA, pa);          /* 动态 tag/payload：store_imm 的 */
            store_tag(j, dstA, ta);          /* tag 是立即数，传 ref 会存 ref-id */
            ir_ref eS = ir_END();
        ir_IF_FALSE(ifs);
            ir_ref th[2] = { dstA, aa };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_inc_olddst), 2, th);
            ir_ref eH = ir_END();
        ir_MERGE_2(eS, eH);
    }
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
        /* 走 call_slow_binop 封装（P0-12：内含 deopt 分支） */
        call_slow_binop(j, op->op == OP_INC ? OP_ADD : OP_SUB, da, ta, pa,
                        ir_CONST_I32(JT_FLOAT),
                        ir_BITCAST_U64(ir_CONST_DOUBLE(fd2)));
        ir_ref eSlow = ir_END();
    ir_MERGE_2(eFast, eSlow);
    if (has_dst && prefix) {
        /* prefix（res=nv）：src 已是新值——标量直存；非标量（如 "abc"++
         * 增量得新串）→ tv_copy（release+copy+addref） */
        ir_ref nt = load_tag(j, aa), np = load_pay(j, aa);
        ir_ref ifp = ir_IF(ir_LT(nt, ir_CONST_I32(5)));
        ir_IF_TRUE(ifp);
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

/* P0-14 批2：调用序/op 回放发射（vm.c tphp_jit_opseq）——表达式已由 JIT
 * 求值；dst 由真 handler 写回 frame 槽，IR 端只做 deopt 门（P0-12 姿样：
 * IF_TRUE RETURN0 / IF_FALSE 留开；无 dst store、无 MERGE）。 */
static void lower_replay(JitC *j, uint32_t pc_start, uint32_t pc_end) {
    JIT_CTX();
    ir_ref t[4] = { j->r_vm, ir_CONST_I32((int32_t)pc_start),
                    ir_CONST_I32((int32_t)pc_end), j->r_call_clo };
    ir_ref ok = ir_CALL_N(IR_I32,
                          ir_CONST_ADDR((uintptr_t)tphp_jit_opseq), 4, t);
    ir_ref ifs = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifs);
        ir_RETURN(ir_CONST_I32(0));
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
            ir_RETURN(ir_CONST_I32(0));   /* deopt: interpreter unwinds */
        ir_IF_FALSE(ifx);
            if (doop->tr != OPD_UNUSED) {
                ir_ref da = slot_addr(j, doop->ar);
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
            ir_RETURN(ir_CONST_I32(0));
        ir_IF_FALSE(ifd);
        ir_ref efast = ir_END();
        ir_MERGE_N((ir_ref)nslow, slow);
        ir_ref t5[5] = { j->r_vm, clo_sc, args_arr,
                         ir_CONST_I32((int32_t)argc), ret_sc };
        ir_ref ok2 = ir_CALL_N(IR_I32,
                               ir_CONST_ADDR((uintptr_t)tphp_jit_call_dyn), 5, t5);
        ir_ref ifd2 = ir_IF(ir_EQ(ok2, ir_CONST_I32(0)));
        ir_IF_TRUE(ifd2);
            ir_RETURN(ir_CONST_I32(0));
        ir_IF_FALSE(ifd2);
        ir_ref eslow = ir_END();
        ir_MERGE_2(eslow, efast);
        if (doop->tr != OPD_UNUSED) {
            ir_ref da = slot_addr(j, doop->ar);
            store_pay(j, da, ir_LOAD_I64(ret_sc));
            store_tag(j, da, ir_LOAD_I32(ir_ADD_A(ret_sc, ir_CONST_ADDR(TVAL_TAG))));
        }
        j->p_dyn = 0;
        j->p_dyn_ic = 0;
        return;
    }
    TFunc *callee = j->p_callee;
    if (!callee || argc != callee->n_params) { j->fail = 1; return; }
    /* callee 编译失败（hot=UINT16_MAX, entry NULL）→ caller 拒编走解释器：
     * 否则运行时 call_slots 返 0 读垃圾 ret（修复既有隐患，compile_callees
     * 只尝试不保证成功）。self 递归跳过（执行时自身 entry 必已设）。 */
    if (callee != j->fn && !callee->jit_entry) { j->fail = 1; return; }
    /* P0-15 bug3：静态方法 TFunc（批2-2 解禁后带 jit_entry）帧布局不同
     * （$this slot0 / 参数从 slot1，poff=1）——直调用函数布局参数错位。
     * 保守回退解释器（与批2-2 前行为一致），方法直调布局留批3-3。 */
    if (callee->is_method) { j->fail = 1; return; }

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
    /* init locals/tmps to UNDEF (params get overwritten below) */
    for (uint32_t k = callee->n_params; k < nslots; k++) {
        ir_ref d = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
        store_imm(j, d, (int32_t)T_UNDEF, ir_CONST_I64(0));
    }
    for (uint32_t k = 0; k < argc; k++) {
        ir_ref d = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
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
        ir_RETURN(ir_CONST_I32(0));
    ir_IF_FALSE(ife);
    for (uint32_t k = 0; k < argc; k++) {
        ir_ref a = ir_ADD_A(slots_sc, ir_CONST_ADDR((uintptr_t)k * TVAL_SZ));
        ir_ref tg = ir_LOAD_I32(ir_ADD_A(a, ir_CONST_ADDR(TVAL_TAG)));
        ir_ref ift = ir_IF(ir_NE(tg, ir_CONST_I32(T_INT)));
        ir_IF_TRUE(ift);
            ir_RETURN(ir_CONST_I32(0));
        ir_IF_FALSE(ift);
    }
    ir_ref dir_t[3] = { j->r_vm, slots_sc, ret_sc };
    ir_ref ok = ir_CALL_N(IR_I32, entry, 3, dir_t);
    /* callee deopt（内含 raise，exception 已设）→ caller 同样立即 RETURN 0
     * 沿 JIT→JIT 链逐层传播，直到解释器 op_do_fcall C 帧 longjmp（不跨 IR）。
     * ok==0 仅此一种可达：entry-NULL 已在上面拒绝，g_jsp 溢出只在
     * tphp_jit_call（dyn 路径）。 */
    ir_ref ifd = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
    ir_IF_TRUE(ifd);
        ir_RETURN(ir_CONST_I32(0));
    ir_IF_FALSE(ifd);
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

static Trap emit_seq(JitC *j, uint32_t pc, const uint32_t *stops, int nstops) {
    JIT_CTX();
    if (j->fail) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
    for (;;) {
        j->cur_pc = pc;
        if (pc >= j->fn->n_ops) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
        if (stop_has(stops, nstops, pc)) return trap_end(ir_END(), pc);

        if (is_backedge_target(j->fn, pc) &&
            !(j->loop_sp > 0 && j->loop_hdr[j->loop_sp - 1] == pc)) {
            if (j->loop_sp >= JIT_MAX_LOOPS) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
            ir_ref ein = ir_END();
            j->loop_ref[j->loop_sp] = ir_LOOP_BEGIN(ein);
            j->loop_hdr[j->loop_sp] = pc;
            j->loop_cond[j->loop_sp] = 1;
            j->lb_np[j->loop_sp] = 0;          /* 批5: 层复用前清 pending/||标记 */
            j->lb_jmpnz[j->loop_sp] = 0;
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
                store_imm(j, slot_addr(j, op->ar), (int32_t)v.t,
                          ir_CONST_I64(v.u.i));
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
                ir_RETURN(ir_CONST_I32(0));
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
            ir_ref t[4] = { j->r_vm, base, key, val };
            ir_ref ok = ir_CALL_N(IR_I32,
                                  ir_CONST_ADDR((uintptr_t)tphp_jit_assign_dim),
                                  4, t);
            /* raise → 0：IR 立即 deopt（bcall 同款，0008 因子式——
             * IF_TRUE RETURN / IF_FALSE 留开继续，绝不跨 IR 帧 longjmp） */
            ir_ref ifx = ir_IF(ir_EQ(ok, ir_CONST_I32(0)));
            ir_IF_TRUE(ifx);
                ir_RETURN(ir_CONST_I32(0));
            ir_IF_FALSE(ifx);
            pc++;
            break;
        }
        case OP_FETCH_DIM_R: {
            JIT_CTX();
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
            g.kt = load_tag(j, g.key);
            g.kp = load_pay(j, g.key);
            ir_ref ifa = ir_IF(ir_EQ(load_tag(j, g.base), ir_CONST_I32(T_ARR)));
            ir_IF_TRUE(ifa);
                g.a = load_pay(j, g.base);
                /* 互斥键路径：packed int 直取 vs hash 快照 */
                ir_ref pcond = ir_AND(IR_BOOL,
                    ir_NE(ir_LOAD_U8(ir_ADD_A(g.a, ir_CONST_ADDR(offsetof(TArr, packed)))),
                          ir_CONST_U8(0)),
                    ir_EQ(g.kt, ir_CONST_I32(JT_INT)));
                ir_ref ifp = ir_IF(pcond);
                ir_IF_TRUE(ifp);
                    /* (u64)key < (u64)n 单条无符号比较：天然覆盖 i<0
                     * （负数 = 巨大无符号）与 i>=n，与 helper 的
                     * i>=0 && (uint32)i<n 等价 */
                    ir_ref n64 = ir_ZEXT(IR_U64,
                        ir_LOAD_U32(ir_ADD_A(g.a, ir_CONST_ADDR(offsetof(TArr, n)))));
                    ir_ref ifb = ir_IF(ir_ULT(ir_BITCAST_U64(g.kp), n64));
                    ir_IF_TRUE(ifb);
                        ir_ref data = ir_LOAD_A(
                            ir_ADD_A(g.a, ir_CONST_ADDR(offsetof(TArr, data))));
                        dimic_take(&g, ir_ADD_A(data,
                            ir_SHL_U64(ir_BITCAST_U64(g.kp), ir_CONST_U64(4))));
                        ir_ref eP1 = ir_END();
                    ir_IF_FALSE(ifb);
                        dimic_miss(&g, false);   /* packed 越界非 IC 键形态 */
                        ir_ref eP0 = ir_END();
                    ir_MERGE_2(eP1, eP0);
                    ir_ref ePack = ir_END();
                ir_IF_FALSE(ifp);
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
                    ir_ref eIc = ir_END();
                ir_MERGE_2(ePack, eIc);
                ir_ref ct = ir_END();
            ir_IF_FALSE(ifa);
                dimic_miss(&g, false);   /* 非数组 TypeError 走 helper 语义 */
                ir_ref cf = ir_END();
            ir_MERGE_2(ct, cf);   /* 主尾活线，emit_seq 继续 */
            pc++;
            break;
        }
        case OP_CLOSURE_INIT: {  /* 无捕获（资格判定 a2==0） */
            JIT_CTX();
            ir_ref args[4] = { j->r_vm, slot_addr(j, op->ar),
                               ir_CONST_ADDR((uintptr_t)j->fn),
                               ir_CONST_U32(op->a1) };
            ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)tphp_jit_closure_init), 4, args);
            pc++;
            break;
        }
        case OP_INIT_DYN: {
            /* 动态调用折叠：$f(...) —— 闭包槽 a1，SEND…DO_FCALL 同静态折叠 */
            if (j->p_callee || j->p_seq) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }  /* nested */
            j->p_dyn = 1;
            j->p_dyn_slot = op->a1;
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
            /* P0-14 批2：new 序起点（回放 op_init_new→SENDs→NEW_USER） */
            if (j->p_seq || j->p_callee || j->p_dyn || j->p_bfn) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
            j->p_seq = 1;
            j->p_seq_pc = pc;
            pc++;
            break;
        }
        case OP_INIT_MCALL: {
            /* $o->m(...) 序起点：recv 快照进 entry scratch（实参表达式可
             * 复用接收者槽）；方法名限字面量（动态名 → fail 回退解释器） */
            if (j->p_seq || j->p_callee || j->p_dyn || j->p_bfn) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
            if (op->tr != OPD_CONST || (op->t1 != OPD_CV && op->t1 != OPD_TMP)) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
            ir_ref ra = slot_addr(j, op->a1);
            store_pay(j, j->r_call_clo, load_pay(j, ra));
            store_tag(j, j->r_call_clo, load_tag(j, ra));
            j->p_seq = 2;
            j->p_seq_pc = pc;
            pc++;
            break;
        }
        case OP_NEW_USER: {
            if (j->p_seq != 1) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
            lower_replay(j, j->p_seq_pc, pc + 1);
            pc++;
            break;
        }
        case OP_FETCH_PROP: case OP_ASSIGN_PROP:
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
                j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0);
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
        case OP_UNSET_CV: {
            /* 解释器 op_unset_cv 同款：释放槽值 + 置 UNDEF（dtor defer 于
             * mem.c，release 不进用户码不 raise → 无需 deopt 门） */
            if (op->t1 != OPD_CV) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
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
            lower_binop(j, op); pc++; break;
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
                ir_RETURN(ir_CONST_I32(0));
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
            if (j->p_callee || j->p_bfn || j->p_seq) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }  /* nested */
            if ((uintptr_t)op->ic & 1)
                j->p_bfn = (void *)((uintptr_t)op->ic & ~(uintptr_t)1);
            else
                j->p_callee = (TFunc *)op->ic;
            j->p_dyn = 0;
            j->p_argc = 0;
            pc++;
            break;
        }
        case OP_SEND_VAL: case OP_SEND_VAR: {
            if (!j->p_callee && !j->p_dyn && !j->p_bfn && !j->p_seq) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
            if (j->p_seq) { pc++; break; }   /* P0-14 批2: 回放中由真 SEND handler 读槽 */
            if (j->p_argc >= 9) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
            j->p_arg_slot[j->p_argc++] = ((uint32_t)op->t1 << 24) | op->a1;
            pc++;
            break;
        }
        case OP_DO_FCALL: {
            if (j->p_seq == 2) {   /* P0-14 批2: mcall 序回放 */
                lower_replay(j, j->p_seq_pc, pc + 1);
                pc++;
                break;
            }
            if (!j->p_callee && !j->p_dyn && !j->p_bfn) { j->fail = 1; do { j->fail_line = __LINE__; return trap_fail(); } while (0); }
            lower_call_finish(j, op);
            if (j->fail) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
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
            if (T <= pc) do { j->fail_line = __LINE__; return trap_fail(); } while (0);

            /* loop condition: first JMPZ at loop top level */
            if (j->loop_sp > 0 && j->loop_cond[j->loop_sp - 1]) {
                j->loop_cond[j->loop_sp - 1] = 0;
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
                    ir_ref le;
                    if (j->lb_np[k] > 0) {
                        ir_ref in[JIT_LB_MAX + 1];
                        int n = j->lb_np[k];
                        for (int bi = 0; bi < n; bi++) in[bi] = j->lb_pend[k][bi];
                        in[n] = ir_END();          /* 体尾回跳区 */
                        j->lb_np[k] = 0;
                        ir_MERGE_N((ir_ref)(n + 1), in);
                        le = ir_LOOP_END();
                    } else {
                        le = ir_LOOP_END();
                    }
                    ir_MERGE_SET_OP(j->loop_ref[k], 2, le);
                } else {
                    do { j->fail_line = __LINE__; return trap_fail(); } while (0);   /* body must back-edge or fail */
                }
                ir_IF_FALSE(ifx);
                /* exit path continues at E inside the false region */
                j->loop_sp--;
                pc = E;
                break;
            }

            uint32_t join, else_start;
            if (op->op == OP_JMPNZ && j->loop_sp > 0)
                j->lb_jmpnz[j->loop_sp - 1] = 1;   /* || 求值标记（挡板因子） */
            if (!find_join(j->fn, pc, T, &join, &else_start))
                do { j->fail_line = __LINE__; return trap_fail(); } while (0);

            ir_ref ta, pa, ad;
            ad = cond_addr(j, op->t1, op->a1, &ta, &pa);
            ir_ref truthy = make_truthy(j, ta, pa, ad);
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
                if (nstops > 10) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = join;
                tA = emit_seq(j, T, st, ns);   /* 真跳目标体 → join */
                if (tA.kind == TR_FAIL) return tA;
                if (tA.kind == TR_LOOPBACK) {
                    if (!lb_push(j, ir_END())) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                    tA = trap_ret();
                }
                if (tA.kind == TR_END && tA.target != join)
                    do { j->fail_line = __LINE__; return trap_fail(); } while (0);
            } else {
                uint32_t st[11]; int ns = nstops + 1;
                if (nstops > 10) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = T;
                tA = emit_seq(j, pc + 1, st, ns);
                if (tA.kind == TR_FAIL) return tA;
                if (tA.kind == TR_LOOPBACK) {
                    /* P0-14 批5: then 内 continue——分支 END 存 pending，
                     * 按终端参与既有 RET 合流（ir_END 已闭合本区） */
                    if (!lb_push(j, ir_END())) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                    tA = trap_ret();
                }
                if (tA.kind == TR_END && tA.target != join &&
                    !(else_start && tA.target == else_start))
                    do { j->fail_line = __LINE__; return trap_fail(); } while (0);
            }

            ir_IF_FALSE(ifx);
            Trap tB;
            if (!jmp_on_true) {
                if (!else_start) {
                    tB = trap_end(ir_END(), T);
                } else {
                    uint32_t st[11]; int ns = nstops + 1;
                    if (nstops > 10) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                    if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                    st[nstops] = join;
                    tB = emit_seq(j, else_start, st, ns);
                    if (tB.kind == TR_FAIL) return tB;
                    if (tB.kind == TR_LOOPBACK) {
                        /* P0-14 批5: else 内 continue——同 tA 转终端 */
                        if (!lb_push(j, ir_END())) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                        tB = trap_ret();
                    }
                    if (tB.kind == TR_END && tB.target != join)
                        do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                }
            } else {
                /* P0-14 批5: JMPNZ 假侧 fallthrough → join（stops 含 T：
                 * 若中途落 T=else_start 则保守 END@T → 合流 fail 回退） */
                uint32_t st[12]; int ns = nstops + 2;
                if (nstops > 10) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                if (nstops) memcpy(st, stops, (size_t)nstops * 4);
                st[nstops] = T;
                st[nstops + 1] = join;
                tB = emit_seq(j, pc + 1, st, ns);
                if (tB.kind == TR_FAIL) return tB;
                if (tB.kind == TR_LOOPBACK) {
                    if (!lb_push(j, ir_END())) do { j->fail_line = __LINE__; return trap_fail(); } while (0);
                    tB = trap_ret();
                }
                if (tB.kind == TR_END && tB.target != join &&
                    !(tB.target == T))
                    do { j->fail_line = __LINE__; return trap_fail(); } while (0);
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
            do { j->fail_line = __LINE__; return trap_fail(); } while (0);
        }
        default:
            do { j->fail_line = __LINE__; return trap_fail(); } while (0);
        }
    }
}

/* ================================================================
 * Compile entry
 * ================================================================ */
/* Compile all direct callees first so their jit_entry is set when this
 * function runs (direct-slot ABI: caller embeds TFunc* and looks up
 * jit_entry at runtime — NULL entry would read garbage ret). */
static void jit_compile_callees(TFunc *fn) {
    for (uint32_t i = 0; i < fn->n_ops; i++) {
        TOp *op = &fn->ops[i];
        if (op->op != OP_INIT_FCALL) continue;
        TFunc *callee = (TFunc *)op->ic;
        if (!op->ic || ((uintptr_t)op->ic & 1)) continue;
        if (callee == fn) continue;           /* self-recursion: entry already set */
        /* 批5: 尊重急切/热阈值——绕过阈值连带编译把大 callee 的编译成本
         * 塞进 caller 首次调用的计时（logstat n_ops=106 被闭包连带编译
         * +85ms 实测）。阈值不满足→不编→下方 lower_call_finish 的
         * `!callee->jit_entry → fail` 让 caller 回退解释器（既有兜底）；
         * callee 被解释器调热(hot≥127)后自然编译，caller 随后可编。 */
        if (!callee->jit_entry && callee->hot != UINT16_MAX &&
            (callee->n_ops <= TPHP_JIT_EAGER_MAX_OPS ||
             callee->hot >= TPHP_JIT_HOT_THRESHOLD))
            tphp_jit_compile(callee);         /* recursive; lock is re-entry safe
                                               * via the jit_entry check below */
    }
}

void tphp_jit_compile(TFunc *fn) {
    if (!g_jit_on || !fn) return;
    if (fn->jit_entry) return;
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
    ir_ctx *ctx = tm_malloc(sizeof(ir_ctx));
    memset(ctx, 0, sizeof *ctx);
    j.ctx = ctx;
    ir_init(ctx, IR_FUNCTION | IR_OPT_FOLDING,
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
            }
        }
        /* O1: skip SCCP. O2/SCCP has a multi-slot CALL bug (deps/ir
         * ir_sccp_transform treats tail slots of 7-arg CALL as real insns
         * and zeros an operand → codegen crash on chain TMP-ADD). See
         * tools/ir_bisect.c repro; restore O2 after upstream fix. */
        entry = ir_jit_compile(ctx, 1, &size);   /* O1 */
        if (jit_dbg())
            fprintf(stderr, "[jit] compile(%s) entry=%p size=%zu\n",
                    fn->name ? tstr_val(fn->name) : "?", entry, size);
    } else {
        if (getenv("TPHP_JIT_DEBUG") && getenv("TPHP_JIT_DEBUG")[0]) {
            uint32_t pc = j.cur_pc < fn->n_ops ? j.cur_pc : 0;
            fprintf(stderr,
                    "[jit] compile(%s) FAILED emit (trap=%d pc=%u op=%u %s L%d)\n",
                    fn->name ? tstr_val(fn->name) : "?", (int)t.kind, j.cur_pc, (unsigned)fn->ops[pc].op, tphp_op_name(fn->ops[pc].op), j.fail_line);
        }
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

