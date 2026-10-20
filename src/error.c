/*
 * TinyPHP.rt - a small PHP runtime
 * error.c - error routing
 *
 * tphp_fatal_src: hard abort (compile errors, internal invariants).
 * tphp_raise: value-layer errors. When the VM is running it installs
 * tphp_raise_hook and converts the error into a Throwable; without a VM
 * (e.g. --dump-opcodes never executes) it degrades to a hard abort.
 */
#include "tphp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

_Thread_local void *tphp_vm_current = NULL;
_Thread_local void (*tphp_raise_hook)(const char *ex_class_name,
                                      const char *fmt, va_list ap) = NULL;

void tphp_fatal_src(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "PHP Fatal error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (file && line > 0)
        fprintf(stderr, " in %s on line %d", file, line);
    fputc('\n', stderr);
    fflush(stdout);
    exit(255);
}

void tphp_raise(const char *ex_class_name, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (tphp_raise_hook) {
        tphp_raise_hook(ex_class_name, fmt, ap);   /* never returns */
        /* NOT REACHED — hook is declared noreturn */
    }
    va_end(ap);
    va_start(ap, fmt);
    fprintf(stderr, "PHP Fatal error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    fflush(stdout);
    exit(255);
}
