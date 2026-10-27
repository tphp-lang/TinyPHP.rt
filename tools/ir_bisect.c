/* via ir_jit_compile (same path as spike) */
#include "ir.h"
#include "ir_builder.h"
#include <stdio.h>
#include <stdint.h>

typedef struct { union { int64_t i; double d; void *p; } u; int t; } TVal;
#define TVAL_SZ 16
#define TVAL_TAG 8
#define JT_INT 3
#define JT_FLOAT 4

static void slow_add(void *vm, int opi, void *dst,
                     int ta, int64_t pa, int tb, int64_t pb) {
    (void)vm; (void)opi; (void)ta; (void)tb; (void)pb;
    *(int64_t *)dst = pa;
}

static void emit_add(ir_ctx *ctx, ir_ref slots, int dst, int a1, int a2) {
    ir_ref aa = ir_ADD_A(slots, ir_CONST_ADDR((uintptr_t)a1 * TVAL_SZ));
    ir_ref da = ir_ADD_A(slots, ir_CONST_ADDR((uintptr_t)dst * TVAL_SZ));
    ir_ref ta = ir_LOAD_I32(ir_ADD_A(aa, ir_CONST_ADDR(TVAL_TAG)));
    ir_ref pa = ir_LOAD_I64(aa);
    ir_ref ba = ir_ADD_A(slots, ir_CONST_ADDR((uintptr_t)a2 * TVAL_SZ));
    ir_ref tb = ir_LOAD_I32(ir_ADD_A(ba, ir_CONST_ADDR(TVAL_TAG)));
    ir_ref pb = ir_LOAD_I64(ba);
    ir_ref both = ir_AND(IR_BOOL, ir_EQ(ta, ir_CONST_I32(JT_INT)),
                         ir_EQ(tb, ir_CONST_I32(JT_INT)));
    ir_ref ifx = ir_IF(both);
    ir_IF_TRUE(ifx);
        ir_ref sum = ir_ADD_I64(pa, pb);
        ir_ref of = ir_LT(ir_AND_I64(ir_XOR_I64(pa, sum),
                                     ir_XOR_I64(pb, sum)),
                          ir_CONST_I64(0));
        ir_ref ifof = ir_IF(of);
        ir_IF_TRUE(ifof);
            ir_ref fd = ir_ADD_D(ir_BITCAST_D(pa), ir_BITCAST_D(pb));
            ir_STORE(da, ir_BITCAST_U64(fd));
            ir_STORE(ir_ADD_A(da, ir_CONST_ADDR(TVAL_TAG)),
                     ir_CONST_I32(JT_FLOAT));
            ir_ref eO = ir_END();
        ir_IF_FALSE(ifof);
            ir_STORE(da, sum);
            ir_STORE(ir_ADD_A(da, ir_CONST_ADDR(TVAL_TAG)),
                     ir_CONST_I32(JT_INT));
            ir_ref eI = ir_END();
        ir_MERGE_2(eO, eI);
        ir_ref eF = ir_END();
    ir_IF_FALSE(ifx);
        ir_ref args[7] = { ir_CONST_ADDR(0), ir_CONST_I32(2), da, ta, pa, tb, pb };
        ir_CALL_N(IR_VOID, ir_CONST_ADDR((uintptr_t)slow_add), 7, args);
        ir_ref eS = ir_END();
    ir_MERGE_2(eF, eS);
}

int main(void) {
    ir_ctx store;
    ir_ctx *ctx = &store;
    ir_consistency_check();
    ir_init(ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN,
            IR_INSNS_LIMIT_MIN);
    ctx->ret_type = IR_I32;
    ir_START();
    ir_ref vm = ir_PARAM(IR_ADDR, "vm", 1);
    ir_ref slots = ir_PARAM(IR_ADDR, "slots", 2);
    ir_ref args = ir_PARAM(IR_ADDR, "args", 3);
    ir_ref ret = ir_PARAM(IR_ADDR, "ret", 4);
    (void)vm; (void)ret;
    for (int i = 0; i < 3; i++) {
        ir_ref av = ir_ADD_A(args, ir_CONST_ADDR((uintptr_t)i * TVAL_SZ));
        ir_ref sv = ir_ADD_A(slots, ir_CONST_ADDR((uintptr_t)i * TVAL_SZ));
        ir_STORE(sv, ir_LOAD_I64(av));
        ir_STORE(ir_ADD_A(sv, ir_CONST_ADDR(TVAL_TAG)),
                 ir_LOAD_I32(ir_ADD_A(av, ir_CONST_ADDR(TVAL_TAG))));
        ir_STORE(av, ir_CONST_I64(0));
        ir_STORE(ir_ADD_A(av, ir_CONST_ADDR(TVAL_TAG)), ir_CONST_I32(11));
    }
    emit_add(ctx, slots, 3, 0, 1);
    emit_add(ctx, slots, 4, 3, 2);
    ir_RETURN(ir_CONST_I32(1));

    fprintf(stderr, "pre ops8=%d\n", (int)ctx->ir_base[66].ops[8]);
    size_t size = 0;
    void *e = ir_jit_compile(ctx, 2, &size);
    fprintf(stderr, "post ops8=%d entry=%p size=%zu\n",
            (int)ctx->ir_base[66].ops[8], e, size);
    if (e) {
        TVal slots_t[8] = { { { .i = 0 }, 11 }, { { .i = 0 }, 11 },
                            { { .i = 0 }, 11 } };
        TVal args_t[3] = { { { .i = 10 }, 3 }, { { .i = 20 }, 3 },
                           { { .i = 30 }, 3 } };
        TVal ret_t = { { .i = 0 }, 0 };
        int r = ((int (*)(void *, void *, void *, void *))e)(0, slots_t,
                                                            args_t, &ret_t);
        printf("r=%d s3=%lld s4=%lld\n", r,
               (long long)slots_t[3].u.i, (long long)slots_t[4].u.i);
    }
    ir_free(ctx);
    return e ? 0 : 1;
}
