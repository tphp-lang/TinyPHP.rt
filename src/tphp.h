/*
 * TinyPHP.rt - a small PHP runtime
 * tphp.h - public header: common types, version, error codes
 */
#ifndef TPHP_H
#define TPHP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- version ---- */
#define TPHP_VERSION "0.0.2"
#define TPHP_VERSION_MAJOR 0
#define TPHP_VERSION_MINOR 0
#define TPHP_VERSION_PATCH 2

/* target triple, injected by build script (-DTPHP_TARGET=...) */
#ifndef TPHP_TARGET
#define TPHP_TARGET "unknown"
#endif

/* ---- global error codes (C layer) ---- */
typedef enum {
    TPHP_OK = 0,
    TPHP_E_COMPILE = 1,   /* load-time error: source rejected before any execution */
    TPHP_E_RUNTIME = 2,   /* runtime error: uncaught throwable during execution */
    TPHP_E_IO = 3,        /* platform I/O failure */
    TPHP_E_NOMEM = 4,
    TPHP_E_ARGS = 5,      /* bad CLI usage */
    TPHP_E_INTERNAL = 6   /* invariant violated; should never happen */
} TPhpStatus;

/* ---- P0-19 round-5: mask-less setjmp for the unwind system ----
 * Plain setjmp() saves the signal mask on Darwin/glibc = a sigprocmask
 * syscall PER ARM (plus sigaltstack bookkeeping) -- sampled as ~20% of VM
 * time on the mac runner (JIT slow-path guards arm on every non-fast-path
 * invocation; P0-19 CI readout 2026-11-02). The guarded regions never
 * alter the signal mask, so use the mask-less flavor everywhere. Flavor
 * consistency is MANDATORY: every arming site and every jump site in the
 * unwind system must use this pair (a _longjmp into a plain-setjmp buffer
 * or vice versa is UB on glibc/Darwin). _setjmp/_longjmp exist on Darwin,
 * glibc; Windows plain setjmp is already mask-free. */
#if defined(__APPLE__) || defined(__unix__) || defined(__unix)
#  define TPHP_SETJMP(env)     _setjmp(env)
#  define TPHP_LONGJMP(env, v) _longjmp((env), (v))
#else
#  define TPHP_SETJMP(env)     setjmp(env)
#  define TPHP_LONGJMP(env, v) longjmp((env), (v))
#endif

const char *tphp_status_str(TPhpStatus s);

/* ---- fatal errors (error.c) ----
 * tphp_fatal_src: hard abort (compile-time errors, internal invariants).
 * tphp_raise: value-layer errors — routed to the VM's exception machinery
 * when the interpreter is running, otherwise hard-abort like above.
 * Both never return (noreturn longjmp via hook / exit). */
void tphp_fatal_src(const char *php_file, int php_line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
void tphp_raise(const char *ex_class_name, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 2, 3)));

/* installed by vm.c while the interpreter runs (NULL otherwise).
 * M5: per-OS-thread — every worker runtime installs its own.
 * Hook never returns (longjmp into VM unwind). */
typedef void (*TphpRaiseHook)(const char *ex_class_name,
                              const char *fmt, va_list ap)
    __attribute__((noreturn));
extern _Thread_local void *tphp_vm_current;
extern _Thread_local TphpRaiseHook tphp_raise_hook;

/* ---- frame/slots TLS 池（vm.c；perf-roadmap L1 帧池化）----
 * 所有 VMFrame 分配/回收必须走这组 API（含 sched 协程根帧、generator），
 * 保证池内复用与 memset 初始化一致。 */
struct VMFrame;
struct VMFrame *tphp_frame_alloc(void);
void tphp_frame_recycle(struct VMFrame *f);          /* 槽位 TVal 须已释放 */
struct TVal;
struct TVal *tphp_slots_alloc(unsigned n);
void tphp_slots_release(struct TVal *p, unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* TPHP_H */
