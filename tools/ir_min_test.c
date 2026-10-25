/* minimal repro of tphp JIT IR patterns — hang bisect
 * (gen() form: builder macros must NOT expand inside main — T8 hang) */
#include "ir.h"
#include "ir_builder.h"
#include <stdio.h>
#include <stdint.h>

typedef struct { union { int64_t i; double d; void *p; } u; int t; } TVal;

void gen(ir_ctx *ctx) {
    ir_START();
    ir_ref vm    = ir_PARAM(IR_ADDR, "vm", 1);
    ir_ref slots = ir_PARAM(IR_ADDR, "slots", 2);
    ir_ref args  = ir_PARAM(IR_ADDR, "args", 3);
    ir_ref ret   = ir_PARAM(IR_ADDR, "ret", 4);
    (void)vm;

    /* guard: args[0].tag == INT else return 0 */
    ir_ref a0 = ir_ADD_A(args, ir_CONST_ADDR(0));
    ir_ref tg = ir_LOAD_I32(ir_ADD_A(a0, ir_CONST_ADDR(8)));
    ir_ref ifg = ir_IF(ir_NE(tg, ir_CONST_I32(3)));
    ir_IF_TRUE(ifg);
        ir_RETURN(ir_CONST_I32(0));
    ir_IF_FALSE(ifg);

    /* move args[0] -> slots[0] */
    ir_ref s0 = ir_ADD_A(slots, ir_CONST_ADDR(0));
    ir_STORE(s0, ir_LOAD_I64(a0));
    ir_STORE(ir_ADD_A(s0, ir_CONST_ADDR(8)), tg);
    ir_STORE(a0, ir_CONST_I64(0));
    ir_STORE(ir_ADD_A(a0, ir_CONST_ADDR(8)), ir_CONST_I32(11));

    /* const 2 into slots[2]; LT -> bool slots[3] (diamond) */
    ir_ref s2 = ir_ADD_A(slots, ir_CONST_ADDR(32));
    ir_STORE(s2, ir_CONST_I64(2));
    ir_STORE(ir_ADD_A(s2, ir_CONST_ADDR(8)), ir_CONST_I32(3));
    ir_ref pa = ir_LOAD_I64(s0);
    ir_ref pb = ir_LOAD_I64(s2);
    ir_ref lt = ir_LT(pa, pb);
    ir_ref s3 = ir_ADD_A(slots, ir_CONST_ADDR(48));
    ir_ref ifc = ir_IF(lt);
    ir_IF_TRUE(ifc);
        ir_STORE(s3, ir_CONST_I64(0));
        ir_STORE(ir_ADD_A(s3, ir_CONST_ADDR(8)), ir_CONST_I32(2));
        ir_ref eT = ir_END();
    ir_IF_FALSE(ifc);
        ir_STORE(s3, ir_CONST_I64(0));
        ir_STORE(ir_ADD_A(s3, ir_CONST_ADDR(8)), ir_CONST_I32(1));
        ir_ref eF = ir_END();
    ir_MERGE_2(eT, eF);

    /* truthy(s3) via tag==TRUE: slots[1] = n or 42 */
    ir_ref t3 = ir_LOAD_I32(ir_ADD_A(s3, ir_CONST_ADDR(8)));
    ir_ref istrue = ir_EQ(t3, ir_CONST_I32(2));
    ir_ref ifj = ir_IF(istrue);
    ir_IF_TRUE(ifj);
        ir_ref s1 = ir_ADD_A(slots, ir_CONST_ADDR(16));
        ir_STORE(s1, ir_LOAD_I64(s0));
        ir_STORE(ir_ADD_A(s1, ir_CONST_ADDR(8)), ir_CONST_I32(3));
        ir_ref eA = ir_END();
    ir_IF_FALSE(ifj);
        ir_ref s1b = ir_ADD_A(slots, ir_CONST_ADDR(16));
        ir_STORE(s1b, ir_CONST_I64(42));
        ir_STORE(ir_ADD_A(s1b, ir_CONST_ADDR(8)), ir_CONST_I32(3));
        ir_ref eB = ir_END();
    ir_MERGE_2(eA, eB);

    ir_ref v = ir_LOAD_I64(ir_ADD_A(slots, ir_CONST_ADDR(16)));
    ir_STORE(ret, v);
    ir_STORE(ir_ADD_A(ret, ir_CONST_ADDR(8)), ir_CONST_I32(3));
    ir_RETURN(ir_CONST_I32(1));
}

int main(void) {
    ir_ctx ctx;
    ir_consistency_check();
    ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN, IR_INSNS_LIMIT_MIN);
    ctx.ret_type = IR_I32;
    gen(&ctx);
    size_t size = 0;
    ir_dump(&ctx, stderr);
    void *entry = ir_jit_compile(&ctx, 2, &size);
    printf("entry=%p size=%zu\n", entry, size);
    if (entry) {
        int (*fn)(void *, void *, void *, void *) = (void *)entry;
        TVal args_t[2] = { { { .i = 7 }, 3 }, { { .i = 0 }, 0 } };
        TVal slots_t[4] = { { { .i = 0 }, 0 } };
        TVal ret_t = { { .i = 0 }, 0 };
        int ok = fn((void *)0, slots_t, args_t, &ret_t);
        printf("ok=%d ret.i=%lld ret.t=%d s1.i=%lld\n",
               ok, (long long)ret_t.u.i, ret_t.t, (long long)slots_t[1].u.i);
    }
    ir_free(&ctx);
    return 0;
}
