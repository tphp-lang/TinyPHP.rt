/* bench_arr_read.c - P0-17③ profile：把 packed 数组读路径拆到 C 层量。
 * 场景（bench-vs-php array-read 同形态：1000 元素 packed int 数组，
 * 1e6 次 $sum += $a[$i]）：阶梯式分项计时，每档差值 = 该项成本。
 *   A floor      裸 data[i].u.i 求和（无任何检查，理论地板）
 *   B +arr-gate  A + 槽 ARR tag 检查 + packed load + key<n 边界（DimIC
 *                packed fast path 的结构检查集）
 *   C +val-tag   B + 值 tag==INT 检查 + key 槽 INT 检查（$i 槽读）
 *   D +sum-wt    C + $sum 写回（tval_release 内联语义 + TVal 两字段 store）
 *   E 实测       tphp JIT 全路径（bench-vs-php 口径，另测）
 *
 * Build（rsp 链接，同 bench_kv.c）：
 *   clang -O2 -Isrc -c tools\bench_arr_read.c -o build\bench_arr_read.o
 *   （rsp 列 build\obj\*.o 减 src_main.o + build\bench_arr_read.o +
 *    libffi.a + 系统库）
 */
#include "value.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

#define N_KEYS 1000
#define N_READ 1000000

static volatile int64_t sink;

int main(void) {
    /* 构造 packed 数组（tarr_new + 顺序 int 快路，与 bench 同款） */
    TVal av = TV_ARR(tarr_new());
    TArr *a = (TArr *)av.u.p;
    for (int32_t i = 0; i < N_KEYS; i++) {
        TVal v = TV_INT(i);
        tarr_set(a, TV_INT(i), v);
    }
    if (!a->packed || a->n != (uint32_t)N_KEYS) {
        printf("SETUP-FAIL packed=%d n=%u\n", a->packed, a->n);
        return 1;
    }
    /* $a 槽（ARR tag 检查对象）、key 槽（$i）与 sum 槽（$sum）——模拟
     * JIT 帧槽 */
    TVal aslot = av;
    TVal kslot = TV_INT(0);
    TVal sslot = TV_INT(0);
    double t0, t1;
    int64_t sum;
    uint32_t i;

    /* ---- A: floor ---- */
    for (int warm = 0; warm < 3; warm++) {
        sum = 0;
        for (i = 0; i < N_READ; i++) {
            TVal *e = &a->data[i % N_KEYS];
            sum += e->u.i;
        }
        sink = sum;
    }
    t0 = now_ms();
    sum = 0;
    for (i = 0; i < N_READ; i++) {
        TVal *e = &a->data[i % N_KEYS];
        sum += e->u.i;
    }
    t1 = now_ms();
    double msA = t1 - t0;
    printf("A floor        %7.2fms  %5.2fns/read  sum=%lld\n",
           msA, msA * 1e6 / N_READ, (long long)sum);

    /* ---- B: +arr-gate（槽 ARR tag + packed + 边界）---- */
    for (int warm = 0; warm < 3; warm++) {
        sum = 0;
        for (i = 0; i < N_READ; i++) {
            if (aslot.t != T_ARR) return 2;
            TArr *aa = (TArr *)aslot.u.p;
            if (!aa->packed) return 2;
            uint32_t k = (uint32_t)i % N_KEYS;
            if ((uint64_t)k >= (uint64_t)aa->n) return 2;
            sum += aa->data[k].u.i;
        }
        sink = sum;
    }
    t0 = now_ms();
    sum = 0;
    for (i = 0; i < N_READ; i++) {
        if (aslot.t != T_ARR) return 2;
        TArr *aa = (TArr *)aslot.u.p;
        if (!aa->packed) return 2;
        uint32_t k = (uint32_t)i % N_KEYS;
        if ((uint64_t)k >= (uint64_t)aa->n) return 2;
        sum += aa->data[k].u.i;
    }
    t1 = now_ms();
    double msB = t1 - t0;
    printf("B +arr-gate    %7.2fms  %5.2fns/read  (+%.2f)\n",
           msB, msB * 1e6 / N_READ, (msB - msA) * 1e6 / N_READ);

    /* ---- C: +val-tag（值 tag==INT + key 槽 INT 检查）---- */
    for (int warm = 0; warm < 3; warm++) {
        sum = 0;
        for (i = 0; i < N_READ; i++) {
            kslot = TV_INT((int64_t)(i % N_KEYS));   /* $i 槽写（循环归纳） */
            if (aslot.t != T_ARR) return 2;
            if (kslot.t != T_INT) return 2;
            TArr *aa = (TArr *)aslot.u.p;
            if (!aa->packed) return 2;
            uint32_t k = (uint32_t)((uint64_t)kslot.u.i % N_KEYS);
            if ((uint64_t)k >= (uint64_t)aa->n) return 2;
            TVal *e = &aa->data[k];
            if (e->t != T_INT) return 2;
            sum += e->u.i;
        }
        sink = sum;
    }
    t0 = now_ms();
    sum = 0;
    for (i = 0; i < N_READ; i++) {
        kslot = TV_INT((int64_t)(i % N_KEYS));       /* $i 槽写（循环归纳） */
        if (aslot.t != T_ARR) return 2;
        if (kslot.t != T_INT) return 2;
        TArr *aa = (TArr *)aslot.u.p;
        if (!aa->packed) return 2;
        uint32_t k = (uint32_t)((uint64_t)kslot.u.i % N_KEYS);
        if ((uint64_t)k >= (uint64_t)aa->n) return 2;
        TVal *e = &aa->data[k];
        if (e->t != T_INT) return 2;
        sum += e->u.i;
    }
    t1 = now_ms();
    double msC = t1 - t0;
    printf("C +val-tag     %7.2fms  %5.2fns/read  (+%.2f)\n",
           msC, msC * 1e6 / N_READ, (msC - msB) * 1e6 / N_READ);

    /* ---- D: +sum-wt（tval_release 内联语义 + TVal store）----
     * JIT 的 wt($sum, new)：release 旧（INT 快路：tag<5 直接 return）
     * + 两字段 store。C 版调真 tval_release（同款内联语义，-O2 展开）。 */
    for (int warm = 0; warm < 3; warm++) {
        sslot = TV_INT(0);
        for (i = 0; i < N_READ; i++) {
            kslot = TV_INT((int64_t)(i % N_KEYS));
            if (aslot.t != T_ARR) return 2;
            if (kslot.t != T_INT) return 2;
            TArr *aa = (TArr *)aslot.u.p;
            if (!aa->packed) return 2;
            uint32_t k = (uint32_t)((uint64_t)kslot.u.i % N_KEYS);
            if ((uint64_t)k >= (uint64_t)aa->n) return 2;
            TVal *e = &aa->data[k];
            if (e->t != T_INT) return 2;
            TVal nv = TV_INT(sslot.u.i + e->u.i);
            tval_release(&sslot);
            sslot = nv;
        }
        sink = sslot.u.i;
    }
    t0 = now_ms();
    sslot = TV_INT(0);
    for (i = 0; i < N_READ; i++) {
        kslot = TV_INT((int64_t)(i % N_KEYS));
        if (aslot.t != T_ARR) return 2;
        if (kslot.t != T_INT) return 2;
        TArr *aa = (TArr *)aslot.u.p;
        if (!aa->packed) return 2;
        uint32_t k = (uint32_t)((uint64_t)kslot.u.i % N_KEYS);
        if ((uint64_t)k >= (uint64_t)aa->n) return 2;
        TVal *e = &aa->data[k];
        if (e->t != T_INT) return 2;
        TVal nv = TV_INT(sslot.u.i + e->u.i);
        tval_release(&sslot);
        sslot = nv;
    }
    t1 = now_ms();
    double msD = t1 - t0;
    printf("D +sum-wt      %7.2fms  %5.2fns/read  (+%.2f)\n",
           msD, msD * 1e6 / N_READ, (msD - msC) * 1e6 / N_READ);

    /* ---- E: +sum-observed（$s 恒 INT 观测：release 免除、值驻寄存器、
     * 循环尾单次写槽）——loop-carried 特化的收益上限 ---- */
    for (int warm = 0; warm < 3; warm++) {
        int64_t sreg = 0;
        for (i = 0; i < N_READ; i++) {
            kslot = TV_INT((int64_t)(i % N_KEYS));
            if (aslot.t != T_ARR) return 2;
            if (kslot.t != T_INT) return 2;
            TArr *aa = (TArr *)aslot.u.p;
            if (!aa->packed) return 2;
            uint32_t k = (uint32_t)((uint64_t)kslot.u.i % N_KEYS);
            if ((uint64_t)k >= (uint64_t)aa->n) return 2;
            TVal *e = &aa->data[k];
            if (e->t != T_INT) return 2;
            sreg += e->u.i;
        }
        sslot = TV_INT(sreg);
        sink = sslot.u.i;
    }
    t0 = now_ms();
    int64_t sreg = 0;
    for (i = 0; i < N_READ; i++) {
        kslot = TV_INT((int64_t)(i % N_KEYS));
        if (aslot.t != T_ARR) return 2;
        if (kslot.t != T_INT) return 2;
        TArr *aa = (TArr *)aslot.u.p;
        if (!aa->packed) return 2;
        uint32_t k = (uint32_t)((uint64_t)kslot.u.i % N_KEYS);
        if ((uint64_t)k >= (uint64_t)aa->n) return 2;
        TVal *e = &aa->data[k];
        if (e->t != T_INT) return 2;
        sreg += e->u.i;
    }
    t1 = now_ms();
    sslot = TV_INT(sreg);
    double msE = t1 - t0;
    printf("E +sum-obs     %7.2fms  %5.2fns/read  (-%.2f vs D)\n",
           msE, msE * 1e6 / N_READ, (msD - msE) * 1e6 / N_READ);
    printf("F measured     tphp JIT full path ~10.4ns/read (bench-vs-php)\n");
    printf("   floor->D breakdown above; D->E gap = IC chain head +\n"
           "   loop structure + register allocation (JIT codegen space)\n");
    return 0;
}
