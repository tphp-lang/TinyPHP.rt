/* bench_kv.c - P0-16 批6 / kv-read 剖面：把字符串键读路径拆到 C 层量。
 * 场景（bench-vs-php kv-read 同形态：8 键小表轮转读）：
 *   1 packed-int   地板（tarr_get packed 直取）
 *   2 str-get      tarr_get 全路径（norm+hash+probe+eq）
 *   3 str-getic    tarr_get_ic 预热后稳态（DimIC cases 链命中）
 *   4 str-manual   手写理想路径（跳 norm，预取 query hash）= 当前布局下限
 *   5 str-stored   模拟 ABucket 存 hash（探针先比 hash 再 key_eq）
 *
 * Build（rsp 链接，src_main.o 除外）：
 *   clang -O2 -Isrc -c tools\bench_kv.c -o build\bench_kv.o
 *   （rsp 列 build\obj\*.o 减 src_main.o + build\bench_kv.o + libffi.a + 系统库）
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

static volatile void *sink;

static void report(const char *name, double t0, double t1, long n) {
    printf("%-14s %8.1f ns/read\n", name, (t1 - t0) * 1e6 / n);
}

int main(void) {
    static const char *KN[8] = {"alpha", "beta", "gamma", "delta",
                                "epsilon", "zeta", "eta", "theta"};
    TVal keys[8];
    TArr *a = tarr_new();
    for (int i = 0; i < 8; i++) {
        keys[i] = TV_STR(tstr_intern(KN[i], strlen(KN[i])));
        tarr_set(a, keys[i], TV_INT(i + 1));
    }
    TArr *p = tarr_new();
    for (int i = 0; i < 8; i++) tarr_append(p, TV_INT(i + 1));

    /* 模拟存 hash：按桶位存 query 键的 hash（构建期算一次） */
    uint32_t bhash[64];
    memset(bhash, 0xff, sizeof bhash);
    for (int i = 0; i < 8; i++) {
        uint32_t h = TSTR_HASH((TStr *)keys[i].u.p);
        uint32_t slot = h & a->hmask;
        /* 与 arr_find 相同的空桶停靠；构建后无 tombstone，直接落位 */
        while (a->buckets[slot].val_off != UINT32_MAX &&
               a->buckets[slot].key.t != T_NULL)
            slot = (slot + 1) & a->hmask;
        bhash[slot] = h;
    }

    const long N = 10000000;
    double t0, t1;

    t0 = now_ms();
    for (long i = 0; i < N; i++)
        sink = tarr_get(p, TV_INT((int64_t)(i & 7)));
    t1 = now_ms();
    report("packed-int", t0, t1, N);

    t0 = now_ms();
    for (long i = 0; i < N; i++)
        sink = tarr_get(a, keys[i & 7]);
    t1 = now_ms();
    report("str-get", t0, t1, N);

    DimIC ic;
    tphp_dim_ic_init(&ic);
    for (int i = 0; i < 8; i++) tarr_get_ic(a, keys[i & 7], &ic);
    t0 = now_ms();
    for (long i = 0; i < N; i++)
        sink = tarr_get_ic(a, keys[i & 7], &ic);
    t1 = now_ms();
    report("str-getic", t0, t1, N);

    t0 = now_ms();
    for (long i = 0; i < N; i++) {
        TVal k = keys[i & 7];
        TStr *ks = (TStr *)k.u.p;
        uint32_t h = TSTR_HASH(ks);
        uint32_t slot = h & a->hmask;
        for (;;) {
            ABucket *b = &a->buckets[slot];
            if (b->key.t == T_NULL) break;              /* never used */
            if (b->val_off != UINT32_MAX && b->key.t == T_STR &&
                tstr_eq((TStr *)b->key.u.p, ks)) {
                sink = &a->data[b->val_off];
                break;
            }
            slot = (slot + 1) & a->hmask;
        }
    }
    t1 = now_ms();
    report("str-manual", t0, t1, N);

    t0 = now_ms();
    for (long i = 0; i < N; i++) {
        TVal k = keys[i & 7];
        TStr *ks = (TStr *)k.u.p;
        uint32_t h = TSTR_HASH(ks);
        uint32_t slot = h & a->hmask;
        for (;;) {
            ABucket *b = &a->buckets[slot];
            if (b->key.t == T_NULL) break;
            if (bhash[slot] == h && b->val_off != UINT32_MAX &&
                tstr_eq((TStr *)b->key.u.p, ks)) {
                sink = &a->data[b->val_off];
                break;
            }
            slot = (slot + 1) & a->hmask;
        }
    }
    t1 = now_ms();
    report("str-stored", t0, t1, N);

    printf("sink=%p hmask=%u\n", sink, a->hmask);
    return 0;
}
