/*
 * TinyPHP.rt - a small PHP runtime
 * jit.h - IR baseline JIT (PLAN/ir-jit.md)
 *
 * v1: function-level, closed scalar world (int/float/bool/null). Eligible
 * hot functions compile to machine code via deps/ir; everything else keeps
 * running on the interpreter forever.
 */
#ifndef TPHP_JIT_H
#define TPHP_JIT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct TFunc TFunc;
typedef struct VM VM;
typedef struct TVal TVal;

/* JIT'd function entry (PLAN/ir-jit.md §3, v1.2 direct-slot ABI):
 *   slots[0..n_params-1] = params (caller guarantees INT for JIT→JIT);
 *   slots[n_params..]    = locals/tmps (caller inits to UNDEF);
 *   1 = *ret holds the owned return value;
 *   0 = deopt (should not happen for JIT→JIT; interpreter path re-runs) */
typedef int (*JitEntry)(VM *vm, TVal *slots, TVal *ret);

/* Hotness threshold for the first compile attempt (zend jit_hot_func default) */
#define TPHP_JIT_HOT_THRESHOLD 127
/* 急切编译（v1.1）：体小的合格函数首调即编译——函数粒度热度对
 * "只调一次的大循环"（bench 闭包形态）永不触发；小体编译成本 ≪ 循环收益 */
#define TPHP_JIT_EAGER_MAX_OPS 96

/* 循环 OSR（PLAN/osr.md 批1）：解释器热循环的第二个入口。back-edge 计数
 * 达阈值后编译循环体子图（header..backedge），slots 与解释器帧同布局 →
 * 零拷贝直传 frame->slots；返回 resume_pc（循环 exit 或失败点），解释器
 * 从该 pc 继续。环境变量 TPHP_JIT_OSR 覆盖阈值（默认 10000）。 */
#define TPHP_JIT_OSR_THRESHOLD 10000

/* Process-wide availability (TPHP_JIT=0 disables) */
bool tphp_jit_available(void);
void tphp_jit_init(void);

/* Attempt compilation (idempotent; failure blights fn->hot = UINT16_MAX) */
void tphp_jit_compile(TFunc *fn);

/* Static eligibility probe (PLAN/ir-jit.md §5) */
bool tphp_jit_eligible(TFunc *fn);

/* JIT→JIT call trampoline (PLAN/ir-jit.md §8) */
int tphp_jit_call(VM *vm, TFunc *fn, TVal *args_in, TVal *ret);

/* ---- 循环 OSR（PLAN/osr.md 批1，op_jmp back-edge 触发）----
 * 编译 fn 的 [header, backedge] 循环体子图，返回 chunk 入口指针（失败返
 * NULL，调用方以 TOp.ic 哨兵熔断该位点）。chunk 是循环级产物、per-site
 * 存储于 back-edge op 的 ic（bit63 tag）——函数里多个热循环各自独立。
 * backedge_cur = back-edge op 的解释器 pc（op_jmp 执行时 frame->pc）。 */
void *tphp_jit_compile_osr(TFunc *fn, uint32_t header, uint32_t backedge,
                           uint32_t backedge_cur);
/* back-edge 计数 + 进 OSR 的统一入口（解释器 op_jmp 调用；slots =
 * frame->slots 由调用方传——VM 对 jit.c 不透明）；返回真 = *resume_pc
 * 有效（OSR 跑完循环或失败点），解释器应把 frame->pc 设为它。 */
bool tphp_osr_backedge(VM *vm, TFunc *fn, void **ic_slot, uint32_t header,
                       uint32_t backedge_cur, TVal *slots,
                       uint32_t *resume_pc);

#endif
