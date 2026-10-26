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

/* JIT'd function entry (PLAN/ir-jit.md §3):
 *   1 = *ret holds the owned return value;
 *   0 = deopt (params/locals untouched — interpreter re-runs safely) */
typedef int (*JitEntry)(VM *vm, TVal *slots, TVal *args_in, TVal *ret);

/* Hotness threshold for the first compile attempt (zend jit_hot_func default) */
#define TPHP_JIT_HOT_THRESHOLD 127
/* 急切编译（v1.1）：体小的合格函数首调即编译——函数粒度热度对
 * "只调一次的大循环"（bench 闭包形态）永不触发；小体编译成本 ≪ 循环收益 */
#define TPHP_JIT_EAGER_MAX_OPS 96

/* Process-wide availability (TPHP_JIT=0 disables; aarch64 v1: off) */
bool tphp_jit_available(void);
void tphp_jit_init(void);

/* Attempt compilation (idempotent; failure blights fn->hot = UINT16_MAX) */
void tphp_jit_compile(TFunc *fn);

/* Static eligibility probe (PLAN/ir-jit.md §5) */
bool tphp_jit_eligible(TFunc *fn);

/* JIT→JIT call trampoline (PLAN/ir-jit.md §8) */
int tphp_jit_call(VM *vm, TFunc *fn, TVal *args_in, TVal *ret);

#endif
