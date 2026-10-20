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
#define TPHP_VERSION "0.0.1"
#define TPHP_VERSION_MAJOR 0
#define TPHP_VERSION_MINOR 0
#define TPHP_VERSION_PATCH 1

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

#ifdef __cplusplus
}
#endif

#endif /* TPHP_H */
