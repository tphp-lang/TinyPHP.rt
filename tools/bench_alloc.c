/* bench_alloc.c - P0-16 batch3 step2: isolate allocator cost from VM.
 * Links the SAME mimalloc objects the tphp build uses.
 * Build: clang -O2 -Ideps/mimalloc/include -Ideps/mimalloc/src
 *        tools/bench_alloc.c build/obj/mi_*.o build/obj/mip_*.o -o build/bench_alloc.exe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef USE_LIBC
/* P0-19 discriminator B: pure libc baseline (macOS libmalloc vs tphp TLS) */
#include <stdlib.h>
#define mi_malloc(sz) malloc(sz)
#define mi_free(p)    free(p)
#else
#include "mimalloc.h"
#endif

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
#include <sys/resource.h>
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
/* P0-19 round-3: user/sys/minflt split -- the mac stats showed 27% kernel
 * time; these lines attribute it (page zero-fill vs syscall) per section. */
static void rusage_line(const char *tag) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("  [%s] user=%.3fs sys=%.3fs minflt=%ld majflt=%ld\n", tag,
           (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6,
           (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6,
           ru.ru_minflt, ru.ru_majflt);
}
#endif
#ifndef _WIN32
#define RUSAGE_AVAIL 1
#else
static void rusage_line(const char *tag) { (void)tag; }
#endif

static volatile void *sink;

/* ---- P0-19 cluster-2 probe: tphp object-alloc wrapper simulation ----
 * Isolates what the tphp path adds over a bare mi_malloc/mi_free pair:
 * gc_list doubly-linked registration on alloc + unlink on free.
 * Delta vs bare bench_pair on the SAME machine = wrapper-layer cost. */
typedef struct GCSim {
    struct GCSim *prev, *next;
} GCSim;
static GCSim *gc_list_sim;

static void bench_wrapper(const char *name, size_t sz, long n) {
    double t0 = now_ms();
    for (long i = 0; i < n; i++) {
        char *p = (char *)mi_malloc(sz);
        GCSim *g = (GCSim *)p;
        g->prev = NULL;
        g->next = gc_list_sim;
        if (gc_list_sim) gc_list_sim->prev = g;
        gc_list_sim = g;
        /* unlink BEFORE touch: the touch would clobber the in-block
         * GCSim pointers (same layout as tphp's real GCHead+user split) */
        if (g->prev) g->prev->next = g->next; else gc_list_sim = g->next;
        if (g->next) g->next->prev = g->prev;
        p[0] = (char)i;
        p[sz - 1] = (char)i;
        sink = p;
        mi_free(p);
    }
    double t1 = now_ms();
    printf("%-14s %6zu B  %8.1f ns/pair (wrapper)\n", name, sz,
           (t1 - t0) * 1e6 / n);
}

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

/* ---- P0-19 round 3: first-touch / page-fault discriminator ----
 * The mac MIMALLOC_SHOW_STATS readout showed 27% kernel time with only
 * 3 mmaps + 53 purges -- prime suspect is first-touch zero-fill on
 * Darwin's 16KB pages (touched 389.8 MiB => ~24K faults; Linux 4KB pages
 * fault 4x more often but each ~10-20x cheaper). Two shapes:
 * - huge: fresh mmap-backed block each round -> EVERY round pays full
 *   first-touch (munmap on free guarantees fresh pages next round).
 * - arena: 2 MiB blocks through the arena -> slice reuse after round 1,
 *   so cold-then-hot curve isolates first-touch from steady arena cost.
 * Per-round ms + rusage deltas (user/sys/minflt) split the cost domain. */
static void bench_growth(const char *name, size_t block, long rounds) {
#ifdef RUSAGE_AVAIL
    rusage_line("growth-before");
#endif
    double t0 = now_ms();
    for (long r = 0; r < rounds; r++) {
        double r0 = now_ms();
        char *p = (char *)mi_malloc(block);
        if (!p) { printf("  oom\n"); return; }
        for (size_t i = 0; i < block; i += 4096) p[i] = (char)(i >> 12);
        p[block - 1] = 1;
        sink = p;
        mi_free(p);
        double r1 = now_ms();
        printf("  %-16s round %2ld  %8.1f ms\n", name, r, r1 - r0);
    }
    double t1 = now_ms();
    printf("%-16s %3zu MiB x %ld rounds  %8.1f ms total\n", name,
           block >> 20, rounds, t1 - t0);
#ifdef RUSAGE_AVAIL
    rusage_line("growth-after");
#endif
}

int main(void) {
    long n = 10000000;
    bench_pair("TStr-4B", 50, n);
    bench_wrapper("TStr-4B", 50, n);
    bench_pair("TStr-16B", 64, n);
    bench_wrapper("TStr-16B", 64, n);
    bench_pair("TArr", 64, n);
    bench_wrapper("TArr", 64, n);
    bench_pair("data-8x16", 128, n);
    bench_pair("buckets-16", 384, n / 2);
    bench_pair("TStr-400B", 432, n / 2);
    bench_arrlit(n / 2);
    bench_growth("growth-huge-32M", (size_t)32 << 20, 8);
    bench_growth("growth-arena-2M", (size_t)2 << 20, 8);
    return 0;
}
