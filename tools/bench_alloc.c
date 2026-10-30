/* bench_alloc.c - P0-16 batch3 step2: isolate allocator cost from VM.
 * Links the SAME mimalloc objects the tphp build uses.
 * Build: clang -O2 -Ideps/mimalloc/include -Ideps/mimalloc/src
 *        tools/bench_alloc.c build/obj/mi_*.o build/obj/mip_*.o -o build/bench_alloc.exe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "mimalloc.h"

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

static void bench_pair(const char *name, size_t sz, long n) {
    double t0 = now_ms();
    for (long i = 0; i < n; i++) {
        char *p = (char *)mi_malloc(sz);
        p[0] = (char)i;                 /* touch one line */
        p[sz - 1] = (char)i;
        sink = p;
        mi_free(p);
    }
    double t1 = now_ms();
    printf("%-14s %6zu B  %8.1f ns/pair\n", name, sz, (t1 - t0) * 1e6 / n);
}

/* tphp arr-lit lifecycle: TArr(64) + data(128, first set) + buckets(384,
 * first find->grow) then free x3 — simulates create+2 sets+unset */
static void bench_arrlit(long n) {
    double t0 = now_ms();
    for (long i = 0; i < n; i++) {
        char *ta = (char *)mi_malloc(64);
        char *dt = (char *)mi_malloc(128);
        char *bk = (char *)mi_malloc(384);
        ta[0] = 1; dt[0] = 2; bk[0] = 3;
        sink = ta; sink = dt; sink = bk;
        mi_free(ta); mi_free(dt); mi_free(bk);
    }
    double t1 = now_ms();
    printf("%-14s %8.1f ns/cycle (3 pairs)\n", "arr-lifecycle", (t1 - t0) * 1e6 / n);
}

int main(void) {
    long n = 10000000;
    bench_pair("TStr-4B", 50, n);
    bench_pair("TStr-16B", 64, n);
    bench_pair("TArr", 64, n);
    bench_pair("data-8x16", 128, n);
    bench_pair("buckets-16", 384, n / 2);
    bench_pair("TStr-400B", 432, n / 2);
    bench_arrlit(n / 2);
    return 0;
}
