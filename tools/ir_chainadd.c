/* 链式 TMP-ADD codegen 崩溃的独立复现（v1.1 阻塞，memory/2026-10-27）
 * 形状 = tphp 的 f($a,$b,$c){ return $a+$b+$c; }：
 *   ADD a,b -> t3; ADD t3,c -> t4; RETURN t4
 * 每个 ADD = diamond（both_int? fast(int 钻石) : slow-call）+ MERGE。
 * 与 add12（单 ADD，可编译）对照，隔离"链式"因素。 */
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
    *(int64_t *)dst = pa;   /* 占位：真 helper 在 tphp 里 */
}

/* 单个 add diamond：镜像 tphp lower_binop（committed 钻石版） */
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

typedef int (*fn_t)(void *, void *);

static void gen(ir_ctx *ctx, int chain) {
    ir_START();
    ir_ref vm = ir_PARAM(IR_ADDR, "vm", 1);
    ir_ref slots = ir_PARAM(IR_ADDR, "slots", 2);
    ir_ref args = ir_PARAM(IR_ADDR, "args", 3);
    ir_ref ret = ir_PARAM(IR_ADDR, "ret", 4);
    (void)vm; (void)ret;
    /* tphp 风格 prologue：逐参 INT guard（TRUE: RETURN 0；FALSE 活流）+ move */
    for (int i = 0; i < 3; i++) {
        ir_ref av = ir_ADD_A(args, ir_CONST_ADDR((uintptr_t)i * TVAL_SZ));
        ir_ref tg = ir_LOAD_I32(ir_ADD_A(av, ir_CONST_ADDR(TVAL_TAG)));
        ir_ref ifg = ir_IF(ir_NE(tg, ir_CONST_I32(JT_INT)));
        ir_IF_TRUE(ifg);
            ir_RETURN(ir_CONST_I32(0));
        ir_IF_FALSE(ifg);
    }
    for (int i = 0; i < 3; i++) {
        ir_ref av = ir_ADD_A(args, ir_CONST_ADDR((uintptr_t)i * TVAL_SZ));
        ir_ref sv = ir_ADD_A(slots, ir_CONST_ADDR((uintptr_t)i * TVAL_SZ));
        ir_STORE(sv, ir_LOAD_I64(av));
        ir_STORE(ir_ADD_A(sv, ir_CONST_ADDR(TVAL_TAG)),
                 ir_LOAD_I32(ir_ADD_A(av, ir_CONST_ADDR(TVAL_TAG))));
        ir_STORE(av, ir_CONST_I64(0));
        ir_STORE(ir_ADD_A(av, ir_CONST_ADDR(TVAL_TAG)), ir_CONST_I32(11));
    }
    /* 布局：0=$a 1=$b 2=$c 3=t3 4=t4 */
    emit_add(ctx, slots, 3, 0, 1);           /* t3 = a+b */
    if (chain)
        emit_add(ctx, slots, 4, 3, 2);       /* t4 = t3+c（链式） */
    else
        emit_add(ctx, slots, 4, 0, 2);       /* t4 = a+c（非链式对照） */
    ir_RETURN(ir_CONST_I32(1));
}

int main(int argc, char **argv) {
    int chain = argc > 1;   /* argv[1] = chain：两个 ADD 链式；否则单 ADD */
    ir_ctx ctx;
    ir_consistency_check();
    ir_init(&ctx, IR_FUNCTION | IR_OPT_FOLDING, IR_CONSTS_LIMIT_MIN,
            IR_INSNS_LIMIT_MIN);
    ctx.ret_type = IR_I32;
    gen(&ctx, chain);

    size_t size = 0;
    void *e = ir_jit_compile(&ctx, 2, &size);
    printf("%s entry=%p size=%zu\n", chain ? "chain" : "single", e, size);
    if (e) {
        TVal slots_t[8] = { { { .i = 0 }, 11 }, { { .i = 0 }, 11 },
                            { { .i = 0 }, 11 } };
        TVal args_t[3] = { { { .i = 10 }, 3 }, { { .i = 20 }, 3 },
                           { { .i = 30 }, 3 } };
        TVal ret_t = { { .i = 0 }, 0 };
        int r = ((int (*)(void *, void *, void *, void *))e)(0, slots_t,
                                                            args_t, &ret_t);
        printf("r=%d slots3=%lld t4=%lld\n", r,
               (long long)slots_t[3].u.i, (long long)slots_t[4].u.i);
    }
    ir_free(&ctx);
    return e ? 0 : 1;
}
