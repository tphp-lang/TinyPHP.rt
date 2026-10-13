/*
 * TinyPHP.rt - fiber.c - fcontext coroutine fibers (M4)
 *
 * boost.context old API (php-src Zend/asm vendored): the context function
 * receives a transfer_t (by reference through RCX on win64) on first
 * activation; transfer.from is the caller's context snapshot to jump back
 * to when suspending. Guard page via PAGE_NOACCESS / PROT_NONE; released
 * stacks are pooled and reused.
 *
 * ASAN fiber annotations follow php-src Zend/zend_fibers.c exactly:
 *   - start_switch(save, target_bounds) BEFORE jump; save stays in the
 *     local of the function that jumped — finish(save) when that same
 *     function's jump returns (we are resumed)
 *   - first activation: finish(NULL, &from_bounds_out)
 *   - dying fiber completion: start(NULL, ...) — no save
 *   - finish outputs refresh the other side's asan_bottom/asan_size
 * Without this, ffi_call's asm on a fiber stack hits 0xC0000005 (ASAN
 * fake-stack desync; M4-a lesson 5 blind spot triggered by M5.5 FFI).
 */
#include "fiber.h"
#include "value.h"
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* ---- ASAN fiber annotation macros (no-op without ASAN) ---- */
#if defined(__SANITIZE_ADDRESS__)
#  define TPHP_ASAN_FIBER 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define TPHP_ASAN_FIBER 1
#  endif
#endif

#ifdef TPHP_ASAN_FIBER
void __sanitizer_start_switch_fiber(void **fake_stack_save,
                                    const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save,
                                     const void **bottom_old,
                                     size_t *size_old);
/* save: &local (keep) or NULL (dying / first arrival) */
#  define ASAN_START(save, bot, sz) \
      __sanitizer_start_switch_fiber((save), (bot), (sz))
#  define ASAN_FINISH(save, pbot, psz) \
      __sanitizer_finish_switch_fiber((save), (pbot), (psz))
#else
static inline void asan_start_stub(void **save, const void *bot, size_t sz) {
    (void)bot; (void)sz;
    if (save) *save = NULL;
}
#  define ASAN_START(save, bot, sz) asan_start_stub((save), (bot), (sz))
#  define ASAN_FINISH(save, pbot, psz) \
      do { (void)(save); if (pbot) *(pbot) = NULL; if (psz) *(psz) = 0; } while (0)
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
    /* ASAN: bounds of THIS fiber's stack as ASAN knows them (php-src
     * stack->asan_pointer/asan_size). Refreshed by finish() outputs. */
    const void *asan_bot;
    size_t asan_sz;
    TFiber *resumer;    /* who switched to us (NULL = main thread) */
};

static _Thread_local TFiber *current_fiber;
/* main thread's ASAN stack bounds (no TFiber host); learned via finish */
static _Thread_local const void *g_main_asan_bot;
static _Thread_local size_t g_main_asan_sz;

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

/* target bounds for start_switch when jumping TO f */
static void fiber_asan_target(TFiber *f, const void **bot, size_t *sz) {
    if (f && f->asan_bot) {
        *bot = f->asan_bot;
        *sz = f->asan_sz;
    } else if (f) {
        *bot = (char *)f->stack + TPHP_GUARD_SIZE;
        *sz = f->stack_size - TPHP_GUARD_SIZE;
    } else {
        *bot = g_main_asan_bot;
        *sz = g_main_asan_sz;
    }
}

/* context-function: runs on the fiber's own stack. First activation gets a
 * transfer whose .data is our TFiber* and .from is the caller to return to. */
static void fiber_entry(transfer_t t) {
    TFiber *self = (TFiber *)t.data;
    self->back = t.from;             /* jump-back target for suspends */

#ifdef TPHP_ASAN_FIBER
    /* first arrival: finish(NULL) + learn FROM (resumer) bounds as ASAN
     * sees them — store on the resumer for later start targets */
    const void *from_b = NULL;
    size_t from_s = 0;
    __sanitizer_finish_switch_fiber(NULL, &from_b, &from_s);
    if (from_b) {
        if (self->resumer) {
            self->resumer->asan_bot = from_b;
            self->resumer->asan_sz = from_s;
        } else {
            g_main_asan_bot = from_b;
            g_main_asan_sz = from_s;
        }
    }
#endif

    self->entry(self);

    /* body returned without suspend: dying completion.
     * start(NULL, ...) — like php-src DEAD status: do not save our fake stack. */
    const void *rb;
    size_t rs;
    fiber_asan_target(self->resumer, &rb, &rs);
    ASAN_START(NULL, rb, rs);
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
    /* php-src: asan_pointer/asan_size seeded from allocation bounds */
    f->asan_bot = (char *)stack + TPHP_GUARD_SIZE;
    f->asan_sz = total - TPHP_GUARD_SIZE;
    f->resumer = NULL;
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

    /* departure: save OUR fake stack locally; target = to's bounds.
     * save is consumed by finish below when THIS jump returns (we resume). */
    const void *to_bot;
    size_t to_sz;
    fiber_asan_target(to, &to_bot, &to_sz);
    void *save = NULL;
    ASAN_START(&save, to_bot, to_sz);

    to->resumer = self;

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

    /* arrival on self: finish with OUR save; outputs refresh `to`'s bounds
     * (php-src: to = transfer.context, update to->stack->asan_*) */
#ifdef TPHP_ASAN_FIBER
    {
        const void *ob = NULL;
        size_t os = 0;
        __sanitizer_finish_switch_fiber(save, &ob, &os);
        if (ob) {
            to->asan_bot = ob;
            to->asan_sz = os;
        }
    }
#endif

    /* resumed: t.from is `to`'s suspended snapshot — remember to switch back */
    to->fctx = t.from;
    return t.data;
}

void *tphp_fiber_suspend(void *payload) {
    TFiber *self = current_fiber;

    /* departure back to resumer; save local — finish when we are resumed */
    const void *rb;
    size_t rs;
    fiber_asan_target(self->resumer, &rb, &rs);
    void *save = NULL;
    ASAN_START(&save, rb, rs);

    transfer_t t = jump_fcontext(self->back, payload);

    /* arrival: resumed — finish OUR save; outputs = resumer's stack bounds */
#ifdef TPHP_ASAN_FIBER
    {
        const void *ob = NULL;
        size_t os = 0;
        __sanitizer_finish_switch_fiber(save, &ob, &os);
        if (ob) {
            if (self->resumer) {
                self->resumer->asan_bot = ob;
                self->resumer->asan_sz = os;
            } else {
                g_main_asan_bot = ob;
                g_main_asan_sz = os;
            }
        }
    }
#endif

    self->back = t.from;    /* newest snapshot for the next suspend */
    return t.data;
}
