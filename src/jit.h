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

/* Process-wide availability (TPHP_JIT=0 disables) */
bool tphp_jit_available(void);
void tphp_jit_init(void);

/* Attempt compilation (idempotent; failure blights fn->hot = UINT16_MAX) */
void tphp_jit_compile(TFunc *fn);

/* Static eligibility probe (PLAN/ir-jit.md §5) */
bool tphp_jit_eligible(TFunc *fn);

/* JIT→JIT call trampoline (PLAN/ir-jit.md §8) */
int tphp_jit_call(VM *vm, TFunc *fn, TVal *args_in, TVal *ret);

#endif
