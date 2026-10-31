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

/* ---- P0-16 批C 决策变体 ---- */

/* MTF（move-to-front）链模拟：命中把 case 移到头。轮转访问下预期退化
 * （每次命中移位，下一个访问键仍在尾部附近）——用数据证实/证伪。
 * 快照层（arr/bk/bpending 门）省略：稳态同表，与 str-getic 同口径。 */
static TVal *getic_mtf(TArr *a, TVal key, ABucket **cases, ABucket *dead) {
    for (int i = 0; i < DIM_IC_CASES; i++) {
        ABucket *b = cases[i];
        if (b != dead && b->val_off != UINT32_MAX &&
            b->key.t == key.t && b->key.u.p == key.u.p) {
            /* interned 指针 eq（harness 字面量键同款）；move-to-front：
             * 0..i 右移一格，hit 插头 */
            ABucket *hit = cases[i];
            for (int j = i; j > 0; j--) cases[j] = cases[j - 1];
            cases[0] = hit;
            return &a->data[b->val_off];
        }
    }
    return NULL;
}

/* hash 直达快照模拟：跳 norm（emit 侧键已 norm 形态）+ 稳态跳 arr/bk
 * 快照门（真实 IR 化时 1 load+cmp），限 4 次探测后落 helper（此处返回
 * NULL 即 helper 等价）。这是"链改 hash 直达"方案的 C 层成本上界采样。 */
static TVal *hashic_get(TArr *a, TVal key) {
    TStr *ks = (TStr *)key.u.p;
    uint32_t slot = ks->hash & a->hmask;
    for (int p = 0; p < 4; p++) {
        ABucket *b = &a->buckets[slot];
        if (b->key.t == T_NULL) return NULL;
        if (b->val_off != UINT32_MAX && b->key.t == T_STR &&
            (TStr *)b->key.u.p == ks)
            return &a->data[b->val_off];
        slot = (slot + 1) & a->hmask;
    }
    return NULL;
}

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

    /* ---- 批C 变体 ---- */
    /* keys hash 预热（intern/set 已算过，保险显式触达） */
    for (int i = 0; i < 8; i++) (void)TSTR_HASH((TStr *)keys[i].u.p);

    for (int K = 2; K <= 8; K *= 2) {          /* K 阶梯：链深度 ~K/2 vs O(1) */
        char name[16];
        DimIC ick;
        tphp_dim_ic_init(&ick);
        for (int i = 0; i < K; i++) tarr_get_ic(a, keys[i], &ick);
        t0 = now_ms();
        for (long i = 0; i < N; i++)
            sink = tarr_get_ic(a, keys[i % K], &ick);
        t1 = now_ms();
        snprintf(name, sizeof name, "getic-K%d", K);
        report(name, t0, t1, N);
    }

    {   /* MTF：8 键轮转（预期不优——命中移位白做）。初始链 = tarr_get_ic
         * FIFO 预热装填后的真实桶指针（与 str-getic 同起点，公平对照） */
        DimIC icm;
        tphp_dim_ic_init(&icm);
        for (int i = 0; i < 8; i++) tarr_get_ic(a, keys[i], &icm);
        t0 = now_ms();
        for (long i = 0; i < N; i++)
            sink = getic_mtf(a, keys[i & 7], icm.cases, &icm.dead);
        t1 = now_ms();
        report("getic-mtf", t0, t1, N);
    }

    {   /* hash 直达快照模拟：K 无关 O(1)，链替代方案的 C 层上界采样 */
        t0 = now_ms();
        for (long i = 0; i < N; i++)
            sink = hashic_get(a, keys[i & 7]);
        t1 = now_ms();
        report("str-hashic", t0, t1, N);
    }

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
