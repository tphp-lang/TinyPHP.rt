/*
 * TinyPHP.rt - fiber.c - fcontext coroutine fibers (M4)
 *
 * boost.context old API (php-src Zend/asm vendored): the context function
 * receives a transfer_t (by reference through RCX on win64) on first
 * activation; transfer.from is the caller's context snapshot to jump back
 * to when suspending. Guard page via PAGE_NOACCESS / PROT_NONE; released
 * stacks are pooled and reused.
 */
#include "fiber.h"
#include "value.h"
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* vendored boost.context assembly */
typedef struct { void *from; void *data; } transfer_t;
extern void *make_fcontext(void *sp, size_t size, void (*fn)(transfer_t));
extern transfer_t jump_fcontext(void *to, void *data);

size_t tphp_fiber_default_stack = 256 * 1024;

#define TPHP_GUARD_SIZE 4096

struct TFiber {
    void  *fctx;        /* context snapshot while suspended */
    void  *back;        /* caller context: first activation's transfer.from */
    void  *stack;
    size_t stack_size;
    void (*entry)(TFiber *self);
    void  *udata;       /* opaque owner pointer (sched.c: TCont*) */
    bool   started;
};

static _Thread_local TFiber *current_fiber;

TFiber *tphp_fiber_current(void) { return current_fiber; }
void tphp_fiber_set_current(TFiber *f) { current_fiber = f; }

/* ---- stack management ---- */

static void *stack_alloc(size_t total) {
#ifdef _WIN32
    void *p = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT,
                           PAGE_READWRITE);
    if (!p) return NULL;
    VirtualProtect(p, TPHP_GUARD_SIZE, PAGE_NOACCESS, &(DWORD){0});
    return p;
#else
    void *p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == (void *)-1) return NULL;
    mprotect(p, TPHP_GUARD_SIZE, PROT_NONE);
    return p;
#endif
}

static void stack_free(void *p, size_t total) {
#ifdef _WIN32
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, total);
#endif
}

/* reuse pool (single-threaded scheduler); nodes are heap-allocated — the
 * stack memory itself stays untouched (its first page is NOACCESS guard) */
typedef struct StackNode { struct StackNode *next; void *stack; size_t size; } StackNode;
static _Thread_local StackNode *stack_pool;
static _Thread_local int pool_count;

static void *pool_take(size_t total) {
    for (StackNode **pp = &stack_pool; *pp; pp = &(*pp)->next) {
        if ((*pp)->size == total) {
            StackNode *n = *pp;
            *pp = n->next;
            pool_count--;
            void *p = n->stack;
            tm_free(n);
            return p;
        }
    }
    return NULL;
}

static void pool_put(void *p, size_t total) {
    if (pool_count >= 64) { stack_free(p, total); return; }
    StackNode *n = tm_malloc(sizeof *n);
    n->stack = p;
    n->size = total;
    n->next = stack_pool;
    stack_pool = n;
    pool_count++;
}

/* ---- fibers ---- */

/* context-function: runs on the fiber's own stack. First activation gets a
 * transfer whose .data is our TFiber* and .from is the caller to return to. */
static void fiber_entry(transfer_t t) {
    TFiber *self = (TFiber *)t.data;
    self->back = t.from;             /* jump-back target for suspends */
    self->entry(self);
    /* body returned without an explicit finish switch: jump back anyway */
    jump_fcontext(self->back, NULL);
}

TFiber *tphp_fiber_new(size_t stack_size, void (*entry)(TFiber *self),
                       void *udata) {
    if (stack_size < 64 * 1024) stack_size = 64 * 1024;
    size_t total = stack_size + TPHP_GUARD_SIZE;
    void *stack = pool_take(total);
    if (!stack) {
        stack = stack_alloc(total);
        if (!stack) return NULL;
    }
    TFiber *f = tm_malloc(sizeof *f);
    f->stack = stack;
    f->stack_size = total;
    f->entry = entry;
    f->udata = udata;
    f->started = false;
    /* context data sits at the stack top; make_fcontext aligns internally */
    f->fctx = make_fcontext((char *)stack + total, total, fiber_entry);
    return f;
}

void *tphp_fiber_data(TFiber *f) { return f ? f->udata : NULL; }

void tphp_fiber_release(TFiber *f) {
    if (!f) return;
    pool_put(f->stack, f->stack_size);
    tm_free(f);
}

void *tphp_fiber_switch(TFiber *to, void *data) {
    TFiber *self = current_fiber;
    transfer_t t;
    if (!to->started) {
        to->started = true;
        TFiber *prev = current_fiber;
        current_fiber = to;
        t = jump_fcontext(to->fctx, to);   /* data = the fiber itself */
        current_fiber = prev;
    } else {
        TFiber *prev = current_fiber;
        current_fiber = to;
        t = jump_fcontext(to->fctx, data);
        current_fiber = prev;
    }
    /* resumed: t.from is `to`'s suspended snapshot — remember to switch back */
    to->fctx = t.from;
    return t.data;
}

void *tphp_fiber_suspend(void *payload) {
    TFiber *self = current_fiber;
    transfer_t t = jump_fcontext(self->back, payload);
    self->back = t.from;    /* newest snapshot for the next suspend */
    return t.data;
}
