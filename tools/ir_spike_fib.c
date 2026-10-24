/*
 * IR JIT spike（perf-roadmap §5）：fib 的 opcode→IR 对应物打样。
 * 验证 deps/ir 在 tphp 工具链下 vendor 构建 + JIT 执行闭环。
 * fib 与 tests/bench/test-bench-fib.php 同款语义（fib(28)=317811）。
 *
 * build (Windows):
 *   IR 对象单独放 build/obj_ir/（勿进 build/obj/——全量 build.cmd 会 *.o
 *   全链进 tphp.exe，ir_spike.o 的 main 与 src_main 冲突）：
 *   clang -O2 -m64 -w -DIR_TARGET_X64 -I deps/ir -c deps/ir/<m>.c -o build/obj_ir/ir_<m>.o
 *   （m ∈ ir,ir_cfg,ir_check,ir_dump,ir_emit,ir_emit_c,ir_gcm,ir_gdb,ir_load,
 *     ir_mem2ssa,ir_patch,ir_ra,ir_ra_simple,ir_save,ir_sccp,ir_strtab）
 *   clang -O2 -m64 -DIR_TARGET_X64 -I deps/ir -c tools/ir_spike_fib.c -o build/obj_ir/ir_spike.o
 *   clang -c tools/chkstk_stub.S -o build/obj_ir/chkstk_stub.o   # 无 libgcc，自带 __chkstk 探测桩
 *   clang build/obj_ir/*.o -o build/ir_spike.exe
 */
#include "ir.h"
#include "ir_builder.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* int64 fib(int64 n, uintptr_t self)：self 为自身入口（IR 递归惯例，
 * 无 C 全局/trampoline——tphp 对应物 = 生成代码可直接 call 自身符号） */
typedef int64_t (*fib_t)(int64_t, uintptr_t);

void gen_fib(ir_ctx *ctx) {
    ir_START();
    ir_ref n = ir_PARAM(IR_I64, "n", 1);
    ir_ref self = ir_PARAM(IR_ADDR, "self", 2);

    /* if (n < 2) return n; */
    ir_ref cond = ir_IF(ir_LT(n, ir_CONST_I64(2)));
    ir_IF_TRUE(cond);
        ir_RETURN(n);
    ir_IF_FALSE(cond);
        ir_ref nm1 = ir_SUB_I64(n, ir_CONST_I64(1));
        ir_ref nm2 = ir_SUB_I64(n, ir_CONST_I64(2));
        ir_ref r1 = ir_CALL_2(IR_I64, self, nm1, self);
        ir_ref r2 = ir_CALL_2(IR_I64, self, nm2, self);
        ir_RETURN(ir_ADD_I64(r1, r2));
}

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(void) {
    ir_consistency_check();
    ir_ctx ctx;
    /* 与 Zend tracing JIT 对照的最小 flag 集：fold + SCCP + GCM（IR_OPT_*） */
    ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING | IR_OPT_CFG,
            IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
    ctx.ret_type = IR_I64;
    gen_fib(&ctx);

    size_t size;
    void *entry = ir_jit_compile(&ctx, 2, &size);
    if (!entry) {
        fprintf(stderr, "ir_jit_compile FAILED\n");
        return 1;
    }
    fib_t fib = (fib_t)entry;

    /* 校验 + 计时（fib(28) 与 tphp/PHP bench 同参数） */
    if (fib(10, (uintptr_t)entry) != 55 || fib(28, (uintptr_t)entry) != 317811) {
        fprintf(stderr, "fib correctness FAILED\n");
        return 1;
    }
    double t0 = now_ms();
    volatile int64_t r = fib(28, (uintptr_t)entry);
    double ms = now_ms() - t0;
    printf("fib(28)=%lld  IR-JIT = %.1f ms  (code %zu bytes)\n",
           (long long)r, ms, size);
    (void)size;

    ir_free(&ctx);
    return 0;
}
