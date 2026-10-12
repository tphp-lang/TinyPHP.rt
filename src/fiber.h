/*
 * TinyPHP.rt - fiber.h - fcontext coroutine fibers (M4)
 *
 * Thin wrapper over the vendored boost.context assembly (Zend/asm style,
 * ms_pe variant on Windows). Each fiber owns a VirtualAlloc'd stack with a
 * guard page; released stacks go into a reuse pool so bulk spawn() does
 * not blow up memory (PLAN/04 risk #9).
 */
#ifndef TPHP_FIBER_H
#define TPHP_FIBER_H

#include <stddef.h>

typedef struct TFiber TFiber;

/* create a fiber that runs entry(self) on its own stack when first switched
 * to. stack_size is the usable size (guard page added internally). udata is
 * an opaque pointer retrievable with tphp_fiber_data() inside entry. */
TFiber *tphp_fiber_new(size_t stack_size, void (*entry)(TFiber *self),
                       void *udata);
void *tphp_fiber_data(TFiber *f);

/* cooperative switch to `to` with a payload; suspends the current fiber.
 * Returns the payload `to` handed back when it later switches to us. */
void *tphp_fiber_switch(TFiber *to, void *data);

/* suspend the CURRENT fiber: jump back to whoever jumped into us last.
 * The back-snapshot is refreshed on every resume (the raw jump target from
 * the first activation goes stale once the scheduler jumps in from a
 * different call site). Returns the payload of the next resume. */
void *tphp_fiber_suspend(void *payload);

/* return a finished fiber's stack to the reuse pool */
void tphp_fiber_release(TFiber *f);

/* the fiber currently running on this thread (NULL on the main thread) */
TFiber *tphp_fiber_current(void);
void tphp_fiber_set_current(TFiber *f);

/* default usable stack size for new fibers */
extern size_t tphp_fiber_default_stack;

#endif /* TPHP_FIBER_H */
