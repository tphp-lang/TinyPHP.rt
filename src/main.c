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
#else
#include <unistd.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
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
/* ffi.c：FFI / FFI\CData 内部类注册（编译前；幂等） */
void tphp_ffi_init(void);
/* exclass.c：内置异常层级注册为 TClass（P1-2，编译前；幂等） */
void tphp_exclass_init(void);
/* vm.c：Closure 内部类注册（P1-3，编译前；幂等） */
void tphp_closure_class_init(void);
/* pack.c：M6.1 原生资产打包（构建 + 启动释放，跨平台） */
#include "pack.h"

static char *read_file(const char *path);

static void print_usage(FILE *out) {
    fprintf(out,
        "tphp %s - a small PHP runtime\n"
        "\n"
        "Usage:\n"
        "  tphp run <file.php> [args...]   run a PHP script\n"
        "  tphp -e '<php code>'            execute a code string (<?php implied)\n"
        "  tphp --dump-opcodes <file>      dump compiled opcodes\n"
        "  --debug                         compile-stage diagnostics on stderr\n"
        "  tphp test <dir>                 run EXPECT tests in directory\n"
        "  tphp phpc <dir> -o <out.c>      pack lib\\*.php into bundle_data.c\n"
        "  tphp build <app.php> --with <p>... -o <app.exe>\n"
        "                                  build single exe with embedded assets\n"
        "  tphp -v, --version              print version\n",
        TPHP_VERSION);
}

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#else
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* --debug 时才打印编译阶段诊断（默认静默，避免误导用户） */
static bool g_debug = false;

#ifdef _WIN32
/* 命令行 UTF-16 → UTF-8 argv：否则 -e/路径里的中文先被系统 ANSI 代码页破坏 */
static void win_argv_to_utf8(int *pargc, char ***pargv) {
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv) return;
    char **nav = (char **)calloc((size_t)wargc + 1, sizeof(char *));
    if (!nav) { LocalFree(wargv); return; }
    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (n <= 0) { nav[i] = (char *)calloc(1, 1); continue; }
        nav[i] = (char *)malloc((size_t)n);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nav[i], n, NULL, NULL);
    }
    LocalFree(wargv);
    *pargc = wargc;
    *pargv = nav;
}

/* UTF-8 → ACP：仅用于 system()/cmd.exe 这类 ANSI 边界 */
static void u8_to_acp(const char *s, char *out, size_t cap) {
    wchar_t w[1024];
    out[0] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 1024)) return;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)cap, NULL, NULL);
}

/* 路径按 UTF-8 解释（Windows 走宽字符，中文路径不破） */
static FILE *fopen_u8(const char *path, const char *mode) {
    wchar_t wp[1024], wm[16];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024)) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 16)) return NULL;
    return _wfopen(wp, wm);
}
#else
#define fopen_u8(path, mode) fopen((path), (mode))
#endif

/* ---- M6-e: tphp test <dir> ----
 * 扫描目录下 test-*.php，子进程运行，比对 // EXPECT: 注释与 stdout。
 * 与 tests/run.ps1 同协议；内置后零依赖（发布单 exe 自测）。 */

typedef struct {
    char **paths;
    int n, cap;
} TestList;

static void tl_add(TestList *t, const char *p) {
    if (t->n >= t->cap) {
        t->cap = t->cap ? t->cap * 2 : 32;
        t->paths = realloc(t->paths, sizeof(char *) * (size_t)t->cap);
    }
    t->paths[t->n] = malloc(strlen(p) + 1);
    strcpy(t->paths[t->n], p);
    t->n++;
}

static bool name_is_test(const char *fn) {
    /* test-*.php */
    size_t n = strlen(fn);
    if (n < 9) return false;
    if (strncmp(fn, "test-", 5) != 0) return false;
    return n >= 4 && strcmp(fn + n - 4, ".php") == 0;
}

static void scan_tests(const char *dir, TestList *t) {
#ifdef _WIN32
    wchar_t wdir[768], pattern[800];
    if (!MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, 768)) return;
    _snwprintf(pattern, 800, L"%s\\*.php", wdir);
    pattern[799] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[520];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                 name, sizeof name, NULL, NULL)) continue;
        if (!name_is_test(name)) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s\\%s", dir, name);
        tl_add(t, path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!name_is_test(e->d_name)) continue;
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        tl_add(t, path);
    }
    closedir(d);
#endif
}

/* 运行一个测试文件：捕获 stdout 到临时文件，与 EXPECT 比对 */
static int run_one_test(const char *self_exe, const char *path,
                        char *fail_reason, size_t reason_cap) {
    char cmd[2048];
    char tmp_out[] = "tphp_test_out_XXXXXX";
#ifdef _WIN32
    /* system()/cmd.exe 走 ANSI：exe 与测试路径转 ACP 后拼命令行 */
    char exe_a[1024], path_a[1024];
    u8_to_acp(self_exe, exe_a, sizeof exe_a);
    u8_to_acp(path, path_a, sizeof path_a);
    int tfd = -1;
    /* 简化：用固定临时名（单进程 test 足够）。
     * 开头必须加 '@'：命令串以引号开头会触发 cmd /c 的旧引号剥离规则
     * （去掉首引号 + 字符串里最后一个引号），导致子命令损坏、重定向不落盘。 */
    snprintf(tmp_out, sizeof tmp_out, "build\\_ttest_out.txt");
    snprintf(cmd, sizeof cmd, "@\"%s\" run \"%s\" > \"%s\" 2>NUL",
             exe_a, path_a, tmp_out);
    int rc = system(cmd);
    /* Windows system() 直接返回子进程 ERRORLEVEL（非 wait 编码） */
    int exit_code = rc == -1 ? -1 : (rc & 0xFF);
    (void)tfd;
#else
    snprintf(tmp_out, sizeof tmp_out, "/tmp/tphp_test_out_XXXXXX");
    int tfd = mkstemp(tmp_out);
    if (tfd >= 0) close(tfd);
    snprintf(cmd, sizeof cmd, "\"%s\" run \"%s\" > \"%s\" 2>/dev/null",
             self_exe, path, tmp_out);
    int rc = system(cmd);
    int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    (void)tfd;
#endif

    /* 读 EXPECT 与输出 */
    FILE *fp = fopen_u8(path, "rb");
    if (!fp) {
        snprintf(fail_reason, reason_cap, "cannot open test");
        return -1;
    }
    char line[1024];
    char **expected = NULL;
    int n_exp = 0, cap_exp = 0;
    int want_exit = 0;
    while (fgets(line, sizeof line, fp)) {
        /* strip \r\n */
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "//", 2) != 0) continue;
        p += 2;
        while (*p == ' ') p++;
        if (strncmp(p, "EXPECT:", 7) == 0) {
            p += 7;
            /* skip at most ONE separator space (match run.ps1 \s?);
             * keep indentation so "  [0]=>" survives */
            if (*p == ' ') p++;
            if (n_exp >= cap_exp) {
                cap_exp = cap_exp ? cap_exp * 2 : 16;
                expected = realloc(expected, sizeof(char *) * (size_t)cap_exp);
            }
            expected[n_exp] = malloc(strlen(p) + 1);
            strcpy(expected[n_exp], p);
            n_exp++;
        } else if (strncmp(p, "EXPECT-EXIT:", 12) == 0) {
            p += 12;
            while (*p == ' ') p++;
            want_exit = atoi(p);
        }
    }
    fclose(fp);

    /* 读实际输出 */
    FILE *of = fopen(tmp_out, "rb");
    if (!of) {
        snprintf(fail_reason, reason_cap, "cannot read output");
        for (int i = 0; i < n_exp; i++) free(expected[i]);
        free(expected);
        return -1;
    }
    char **got = NULL;
    int n_got = 0, cap_got = 0;
    while (fgets(line, sizeof line, of)) {
        size_t L = strlen(line);
        while (L && (line[L-1] == '\n' || line[L-1] == '\r')) line[--L] = 0;
        if (n_got >= cap_got) {
            cap_got = cap_got ? cap_got * 2 : 16;
            got = realloc(got, sizeof(char *) * (size_t)cap_got);
        }
        got[n_got] = malloc(strlen(line) + 1);
        strcpy(got[n_got], line);
        n_got++;
    }
    fclose(of);
    remove(tmp_out);

    int ok = 1;
    fail_reason[0] = 0;
    if (exit_code != want_exit) {
        ok = 0;
        snprintf(fail_reason, reason_cap, "exit=%d want=%d", exit_code, want_exit);
    } else if (n_exp != n_got) {
        ok = 0;
        snprintf(fail_reason, reason_cap, "lines %d vs %d", n_got, n_exp);
    } else {
        for (int i = 0; i < n_exp; i++) {
            if (strcmp(expected[i], got[i]) != 0) {
                ok = 0;
                snprintf(fail_reason, reason_cap,
                         "line %d: expected [%s] got [%s]", i + 1, expected[i], got[i]);
                break;
            }
        }
    }
    for (int i = 0; i < n_exp; i++) free(expected[i]);
    free(expected);
    for (int i = 0; i < n_got; i++) free(got[i]);
    free(got);
    return ok;
}

static int run_test_dir(const char *self_exe, const char *dir) {
    TestList t = {0};
    scan_tests(dir, &t);
    if (t.n == 0) {
        fprintf(stderr, "tphp: no test-*.php found in '%s'\n", dir);
        return (int)TPHP_E_ARGS;
    }
    int pass = 0, fail = 0;
    for (int i = 0; i < t.n; i++) {
        char reason[512];
        int ok = run_one_test(self_exe, t.paths[i], reason, sizeof reason);
        const char *base = strrchr(t.paths[i], '/');
        if (!base) base = strrchr(t.paths[i], '\\');
        base = base ? base + 1 : t.paths[i];
        if (ok == 1) {
            pass++;
            printf("PASS %s\n", base);
        } else {
            fail++;
            printf("FAIL %s (%s)\n", base, reason);
        }
        free(t.paths[i]);
    }
    free(t.paths);
    printf("\n%d passed, %d failed\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

/* ---- M6-f: tphp phpc <dir> -o <out.c> ----
 * 扫描目录下 *.php，把源码编成 C 数组写入 out.c（bundle_data 格式）。
 * 模块名 = 文件名去 .php；require 时按名查表。 */

static void emit_c_string(FILE *out, const char *s, size_t n) {
    fputc('"', out);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out); break;
        case '\r': fputs("\\r", out); break;
        case '\t': fputs("\\t", out); break;
        default:
            if (c >= 32 && c < 127)
                fputc(c, out);
            else
                fprintf(out, "\\%03o", c);   /* 八进制：不与后续 hex 粘连 */
        }
    }
    fputc('"', out);
}

static int run_phpc(const char *dir, const char *out_path) {
#ifdef _WIN32
    char pattern[512];
    snprintf(pattern, sizeof pattern, "%s\\*.php", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "tphp: no .php files in '%s'\n", dir);
        return (int)TPHP_E_IO;
    }
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "tphp: cannot write '%s'\n", out_path);
        FindClose(h);
        return (int)TPHP_E_IO;
    }
    fprintf(out, "/* generated by tphp phpc - do not edit */\n");
    fprintf(out, "#include \"bundle.h\"\n\n");
    int count = 0;
    /* 先收集再输出表（需要 count）——两遍：先数，再写 */
    /* 简化：先写占位，最后回写 count 不便——用动态收集 */
    char **names = NULL;
    char **srcs = NULL;
    size_t *lens = NULL;
    int n = 0, cap = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        size_t fl = strlen(fd.cFileName);
        if (fl < 5 || strcmp(fd.cFileName + fl - 4, ".php") != 0) continue;
        char path[512];
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        char *src = read_file(path);
        if (!src) { fclose(out); FindClose(h); return (int)TPHP_E_IO; }
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            names = realloc(names, sizeof(char *) * (size_t)cap);
            srcs = realloc(srcs, sizeof(char *) * (size_t)cap);
            lens = realloc(lens, sizeof(size_t) * (size_t)cap);
        }
        /* 模块名 = 去 .php */
        size_t nl = fl - 4;
        names[n] = malloc(nl + 1);
        memcpy(names[n], fd.cFileName, nl);
        names[n][nl] = 0;
        srcs[n] = src;
        lens[n] = strlen(src);
        n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    for (int i = 0; i < n; i++) {
        fprintf(out, "static const char bundle_src_%d[] = ", i);
        emit_c_string(out, srcs[i], lens[i]);
        fprintf(out, ";\n");
    }
    fprintf(out, "\nconst TBundle tphp_bundles[] = {\n");
    for (int i = 0; i < n; i++)
        fprintf(out, "    { \"%s\", bundle_src_%d, sizeof(bundle_src_%d) - 1 },\n",
                names[i], i, i);
    fprintf(out, "    { NULL, NULL, 0 }\n};\n");
    fprintf(out, "const int tphp_bundle_count = %d;\n", n);
    fclose(out);
    for (int i = 0; i < n; i++) {
        free(names[i]);
        free(srcs[i]);
    }
    free(names);
    free(srcs);
    free(lens);
    printf("phpc: %d module(s) -> %s\n", n, out_path);
    return (int)TPHP_OK;
#else
    (void)dir; (void)out_path;
    fprintf(stderr, "tphp: phpc is Windows-only in M6\n");
    return (int)TPHP_E_ARGS;
#endif
}

static char *read_file(const char *path) {
    FILE *fp = fopen_u8(path, "rb");
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
    tphp_ffi_init();              /* M5.5 FFI 内部类 */
    tphp_exclass_init();          /* P1-2: 内置异常层级（用户异常类可 extends） */
    tphp_closure_class_init();    /* P1-3: Closure 内部类 */
    if (g_debug) fprintf(stderr, "DIAG:init-ok\n");
    AStmtList *ast = tphp_parse_script(src, name);
    if (g_debug) fprintf(stderr, "DIAG:parse-ok stmts=%d\n", ast ? ast->n : -1);
    Program *prog = tphp_compile(ast, name);
    if (g_debug) fprintf(stderr, "DIAG:compile-ok funcs=%d\n", prog ? prog->n_funcs : -1);
    tphp_prog_freeze(prog);       /* M5: 类表冻结 + ic 急解析（跨线程只读） */
    if (g_debug) fprintf(stderr, "DIAG:freeze-ok\n");
    if (dump) {
        tphp_dump_program(prog);
        if (g_debug) fprintf(stderr, "DIAG:dump-ok\n");
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
    /* Windows 控制台默认 GBK(CP936)，而源码/输出是 UTF-8 → 切到 UTF-8 避免中文乱码。
     * 无控制台时（重定向/CI）调用是 no-op，字节已原样写出。 */
    SetConsoleOutputCP(CP_UTF8);
    /* -e/路径里的中文先被 ANSI 破坏 → 命令行改按 UTF-8 解析 */
    win_argv_to_utf8(&argc, &argv);
#endif
    /* line-buffered stdout: when piped (tests/CI) each \n flushes, so a
     * later SIGSEGV still leaves the lines already printed visible */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* --debug 可出现在任意位置，故先全量预扫（不消耗 argv） */
    for (int di = 1; di < argc; di++)
        if (strcmp(argv[di], "--debug") == 0) { g_debug = true; break; }

    /* M6.1: 启动最早期——释放内嵌原生资产 + 检测嵌入主脚本（跨平台） */
    char *embedded_main = NULL;
    size_t embedded_len = 0;
    tphp_pack_startup(&embedded_main, &embedded_len);

    if (argc < 2) {
        /* 打包应用：无参数时跑嵌入主脚本 */
        if (embedded_main) {
            int rc = run_source(embedded_main, "<embedded>", false);
            free(embedded_main);
            return rc;
        }
        free(embedded_main);
        print_usage(stderr);
        return (int)TPHP_E_ARGS;
    }
    free(embedded_main);   /* 有 CLI 参数时不用嵌入脚本 */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--debug") == 0) continue;   /* 已在预扫处理 */

        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            printf("tphp %s (%s, libuv %s)\n", TPHP_VERSION, TPHP_TARGET,
                   uv_version_string());
            return (int)TPHP_OK;
        }
        if (strcmp(a, "build") == 0) {
            /* tphp build <app.php> --with <path>... -o <app> */
            const char *script = NULL;
#ifdef _WIN32
            const char *out_exe = "app.exe";
#else
            const char *out_exe = "app";
#endif
            char *withs[32];
            int nwith = 0;
            for (int j = i + 1; j < argc; j++) {
                if (strcmp(argv[j], "-o") == 0 && j + 1 < argc) {
                    out_exe = argv[++j];
                } else if (strcmp(argv[j], "--with") == 0 && j + 1 < argc) {
                    if (nwith < 32) withs[nwith++] = argv[++j];
                } else if (!script) {
                    script = argv[j];
                }
            }
            if (!script) {
                fprintf(stderr, "tphp: build requires a script path\n");
                return (int)TPHP_E_ARGS;
            }
            char *php = read_file(script);
            if (!php) return (int)TPHP_E_IO;
            /* 复制自身 → out_exe 再注入 */
            char self[1024];
            self[0] = 0;
#ifdef _WIN32
            wchar_t wself[MAX_PATH], wout[MAX_PATH];
            GetModuleFileNameW(NULL, wself, MAX_PATH);
            MultiByteToWideChar(CP_UTF8, 0, out_exe, -1, wout, MAX_PATH);
            if (!CopyFileW(wself, wout, FALSE)) {
                fprintf(stderr, "tphp: cannot copy to '%s' (err %lu)\n",
                        out_exe, GetLastError());
                free(php);
                return (int)TPHP_E_IO;
            }
#else
            /* self_exe_path 在 pack.c——这里用 /proc 或 _NSGetExecutablePath 的简化版 */
#ifdef __linux__
            { ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
              if (n > 0) self[n] = 0; else self[0] = 0; }
#elif defined(__APPLE__)
            { uint32_t sz = sizeof self;
              if (_NSGetExecutablePath(self, &sz) != 0) self[0] = 0; }
#endif
            if (!self[0]) {
                fprintf(stderr, "tphp: cannot resolve own path\n");
                free(php);
                return (int)TPHP_E_IO;
            }
            {
                /* 简单复制 */
                uint8_t *buf = NULL; size_t blen = 0;
                FILE *in = fopen(self, "rb");
                if (!in) { free(php); return (int)TPHP_E_IO; }
                fseek(in, 0, SEEK_END); long sz = ftell(in); fseek(in, 0, SEEK_SET);
                buf = malloc((size_t)sz);
                blen = fread(buf, 1, (size_t)sz, in);
                fclose(in);
                FILE *o = fopen(out_exe, "wb");
                if (!o) { free(buf); free(php); return (int)TPHP_E_IO; }
                fwrite(buf, 1, blen, o);
                fclose(o);
                free(buf);
                chmod(out_exe, 0755);
            }
#endif
            int rc = tphp_pack_exe(out_exe, php, strlen(php), withs, nwith);
            free(php);
            return rc == 0 ? (int)TPHP_OK : (int)TPHP_E_IO;
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
        if (strcmp(a, "test") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tphp: 'test' requires a directory path\n");
                return (int)TPHP_E_ARGS;
            }
            /* 自身 exe 路径：argv[0] */
            return run_test_dir(argv[0], argv[i + 1]);
        }
        if (strcmp(a, "phpc") == 0) {
            /* tphp phpc <dir> -o <out.c> */
            const char *dir = NULL, *outp = "src\\bundle_data.c";
            for (int j = i + 1; j < argc; j++) {
                if (strcmp(argv[j], "-o") == 0 && j + 1 < argc) {
                    outp = argv[++j];
                } else if (!dir) {
                    dir = argv[j];
                }
            }
            if (!dir) {
                fprintf(stderr, "tphp: phpc requires a directory\n");
                return (int)TPHP_E_ARGS;
            }
            return run_phpc(dir, outp);
        }
        if (strcmp(a, "-e") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "tphp: '-e' requires a code string\n");
                return (int)TPHP_E_ARGS;
            }
            /* -r 风格：无 <? 标签时自动补 "<?php "（对齐文档示例与 php -r 直觉） */
            const char *code = argv[i + 1];
            while (*code == ' ' || *code == '\t' || *code == '\r' || *code == '\n') code++;
            int rc;
            if (strncmp(code, "<?", 2) == 0) {
                rc = run_source(argv[i + 1], "<eval>", false);
            } else {
                size_t n = strlen(argv[i + 1]);
                char *wrapped = (char *)malloc(n + 8);
                memcpy(wrapped, "<?php ", 6);
                memcpy(wrapped + 6, argv[i + 1], n + 1);
                rc = run_source(wrapped, "<eval>", false);
                free(wrapped);
            }
            return rc;
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
