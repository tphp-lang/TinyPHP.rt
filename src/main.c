/*
 * TinyPHP.rt - a small PHP runtime
 * main.c - CLI entry: tphp run <file> | -e <code> | --dump-opcodes <file>
 *
 * Pipeline: read file -> lex/parse (AST) -> compile (opcodes) -> run (VM)
 */
#include "tphp.h"
#include "ast.h"
#include "opcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

/* compile.c */
Program *tphp_compile(AStmtList *ast, const char *file);
/* vm.c */
int tphp_vm_run(Program *prog);
void tphp_dump_program(Program *prog);
/* M5: 编译后冻结 prog（预 flatten 全类 + INIT_FCALL ic 急解析） */
void tphp_prog_freeze(Program *prog);
/* sched.c：Async\Coroutine 等内部类注册（编译前；幂等） */
void tphp_async_init(void);
/* io.c：TStream 内部类注册（编译前；幂等） */
void tphp_io_init(void);
/* worker.c：tphp\Worker / WorkerContext 内部类注册（编译前；幂等） */
void tphp_worker_init(void);

static void print_usage(FILE *out) {
    fprintf(out,
        "tphp %s - a small PHP runtime\n"
        "\n"
        "Usage:\n"
        "  tphp run <file.php> [args...]   run a PHP script\n"
        "  tphp -e '<php code>'            execute a code string\n"
        "  tphp --dump-opcodes <file>      dump compiled opcodes\n"
        "  tphp -v, --version              print version\n",
        TPHP_VERSION);
}

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "tphp: could not open '%s'\n", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[got] = 0;
    return buf;
}

static int run_source(const char *src, const char *name, bool dump) {
    tphp_worker_init();           /* tphp\Worker 内部类先于编译注册 */
    tphp_async_init();            /* 内部类先于编译期类检查注册 */
    tphp_io_init();
    AStmtList *ast = tphp_parse_script(src, name);
    Program *prog = tphp_compile(ast, name);
    tphp_prog_freeze(prog);       /* M5: 类表冻结 + ic 急解析（跨线程只读） */
    if (dump) {
        tphp_dump_program(prog);
        return 0;
    }
    int rc = tphp_vm_run(prog);   /* 0 = ok, 255 = uncaught exception */
    fflush(stdout);
    return rc;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    /* binary stdout: PHP CLI parity — echo "\n" must emit LF, not CRLF.
     * Otherwise source bytes containing CRLF get doubled (\r\r\n) on write. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc < 2) {
        print_usage(stderr);
        return (int)TPHP_E_ARGS;
    }

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("tphp %s (%s, libuv %s)\n", TPHP_VERSION, TPHP_TARGET,
                   uv_version_string());
            return (int)TPHP_OK;
        }
        if (strcmp(a, "run") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tphp: 'run' requires a script path\n");
                return (int)TPHP_E_ARGS;
            }
            char *src = read_file(argv[i + 1]);
            if (!src) return (int)TPHP_E_IO;
            int rc = run_source(src, argv[i + 1], false);
            free(src);
            return rc;
        }
        if (strcmp(a, "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tphp: '-e' requires a code string\n");
                return (int)TPHP_E_ARGS;
            }
            return run_source(argv[i + 1], "<eval>", false);
        }
        if (strcmp(a, "--dump-opcodes") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tphp: '--dump-opcodes' requires a script path\n");
                return (int)TPHP_E_ARGS;
            }
            char *src = read_file(argv[i + 1]);
            if (!src) return (int)TPHP_E_IO;
            int rc = run_source(src, argv[i + 1], true);
            free(src);
            return rc;
        }
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return (int)TPHP_OK;
        }

        /* bare script path: implicit 'run' */
        if (a[0] != '-') {
            char *src = read_file(a);
            if (!src) return (int)TPHP_E_IO;
            int rc = run_source(src, a, false);
            free(src);
            return rc;
        }

        fprintf(stderr, "tphp: unknown option '%s'\n", a);
        print_usage(stderr);
        return (int)TPHP_E_ARGS;
    }

    return (int)TPHP_OK;
}

const char *tphp_status_str(TPhpStatus s) {
    switch (s) {
    case TPHP_OK:        return "ok";
    case TPHP_E_COMPILE: return "compile error";
    case TPHP_E_RUNTIME: return "runtime error";
    case TPHP_E_IO:      return "io error";
    case TPHP_E_NOMEM:   return "out of memory";
    case TPHP_E_ARGS:    return "bad usage";
    case TPHP_E_INTERNAL:return "internal error";
    }
    return "unknown";
}
