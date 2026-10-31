/* setjmp/longjmp pair cost on this toolchain (Windows x64, llvm-mingw).
 * Discriminates the throw-path hypothesis: tphp raise = 1 longjmp per
 * throw; if the pair costs ~1us on Windows vs ~15ns on Linux, the
 * same-frame-catch fast path (no longjmp) is the fix. Build:
 *   clang -O2 bench_longjmp.c -o bench_longjmp.exe */
#include <setjmp.h>
#include <stdio.h>
#include <time.h>

static jmp_buf g_jb;

int main(void) {
    volatile long long n = 0;
    /* warm */
    if (setjmp(g_jb) == 0) longjmp(g_jb, 1);
    for (int r = 0; r < 5; r++) {
        clock_t a = clock();
        long long iters = 1000000;
        for (long long i = 0; i < iters; i++) {
            if (setjmp(g_jb) == 0) {
                longjmp(g_jb, 1);
            }
            n++;
        }
        clock_t b = clock();
        double ms = (double)(b - a) * 1000.0 / CLOCKS_PER_SEC;
        printf("setjmp+longjmp pair: %8.1f ms / %lld = %6.1f ns  (n=%lld)\n",
               ms, iters, ms * 1e6 / (double)iters, n);
    }
    return 0;
}
