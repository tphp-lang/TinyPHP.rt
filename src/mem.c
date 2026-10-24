/*
 * TinyPHP.rt - a small PHP runtime
 * mem.c - allocation wrapper (mimalloc via PLAN/03 §7: own layer, no CRT
 *         interpose) + mark-sweep GC that reclaims reference cycles.
 *
 * Every heap value object (TStr/TArr) carries a GCHead and registers itself
 * in a global doubly-linked list. tval_release handles acyclic garbage
 * immediately; gc_collect() finds objects that are only kept alive by
 * reference cycles (php-src style "rc simulation + dfs mark" simplified).
 *
 * Roots: the VM installs its frames/globals via gc_roots_* before collecting
 * (see vm.c). Interned strings never participate.
 */
#include "value.h"
#include "tphp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TPHP_USE_MIMALLOC
#include <mimalloc.h>
#define TM_ALLOC_FN  mi_malloc
#define TM_REALLOC_FN mi_realloc
#define TM_FREE_FN   mi_free
#else
/* ASAN/debug builds: plain CRT allocator (mimalloc and ASAN are mutually
 * exclusive, per PLAN/03 §7) */
#define TM_ALLOC_FN  malloc
#define TM_REALLOC_FN realloc
#define TM_FREE_FN   free
#endif

void *tm_malloc(size_t n) {
    void *p = TM_ALLOC_FN(n);
    if (!p) { fprintf(stderr, "tphp: out of memory (%zu bytes)\n", n); exit((int)TPHP_E_NOMEM); }
    return p;
}

void *tm_realloc(void *p, size_t n) {
    void *q = TM_REALLOC_FN(p, n);
    if (!q) { fprintf(stderr, "tphp: out of memory (%zu bytes)\n", n); exit((int)TPHP_E_NOMEM); }
    return q;
}

void tm_free(void *p) { TM_FREE_FN(p); }

/* ---- registry (per-OS-thread, M5: each worker runtime owns its heap) ---- */
static _Thread_local GCHead *gc_list = NULL;

void gc_register(GCHead *h) {
    h->gc_prev = NULL;
    h->gc_next = gc_list;
    if (gc_list) gc_list->gc_prev = h;
    gc_list = h;
}

/* registry head (vm.c shutdown dtor walk) */
GCHead *tphp_gc_registry(void) { return gc_list; }

static void gc_unregister(GCHead *h) {
    if (h->gc_prev) h->gc_prev->gc_next = h->gc_next; else gc_list = h->gc_next;
    if (h->gc_next) h->gc_next->gc_prev = h->gc_prev;
    h->gc_prev = h->gc_next = NULL;
}

/* ---- string allocation (used by string.c) ---- */
TStr *tstr_alloc_raw(uint32_t len, bool interned) {
    /* 非 interned 预留尾部余量（.=` in-place 扩容，perf-roadmap；zend
     * zend_string rc=1 原地增长同思路）。interned 只读 cap=len。 */
    uint32_t cap = interned ? len : (len < 64 ? 96 : len + len / 2 + 32);
    TStr *s = tm_malloc(sizeof(TStr) + (size_t)cap + 1);
    s->h.rc = 1;
    s->h.color = GC_COLOR_UNMARKED;
    s->h.kind = GC_KIND_STR;
    s->h.flags = interned ? GC_FLAG_INTERNED : 0;
    s->len = len;
    s->hash = 0;
    s->cap = cap;
    s->c[len] = 0;
    if (!interned) gc_register(&s->h);
    return s;
}

TArr *tarr_alloc_raw(void) {
    TArr *a = tm_malloc(sizeof(TArr));
    a->h.rc = 1;
    a->h.color = GC_COLOR_UNMARKED;
    a->h.kind = GC_KIND_ARR;
    a->h.flags = 0;
    a->n = 0; a->bcap = 0; a->hmask = 0;
    a->next_idx = 0;
    a->buckets = NULL; a->data = NULL;
    a->dcap = a->dused = 0;
    a->free_head = UINT32_MAX;
    a->packed = true;   /* 空数组视为 packed（纯追加时保持） */
    gc_register(&a->h);
    return a;
}

void tstr_free(TStr *s) { gc_unregister(&s->h); tm_free(s); }

void tarr_free(TArr *a) {
    /* release element values and key strings */
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF) continue;   /* free slot */
        tval_release(&a->data[i]);
    }
    for (uint32_t i = 0; i < a->bcap; i++) {
        if (a->buckets[i].val_off != UINT32_MAX && a->buckets[i].key.t == T_STR)
            tval_release(&a->buckets[i].key);
    }
    tm_free(a->buckets);
    tm_free(a->data);
    gc_unregister(&a->h);
    tm_free(a);
}

void tclo_free(TClosure *c) {
    for (uint16_t i = 0; i < c->n_cap; i++)
        tval_release(&c->captures[i]);
    tm_free(c->captures);
    gc_unregister(&c->h);
    tm_free(c);
}

/* ---- __destruct (P0-2, 2026-10-15) ----
 * rc 归零释放对象前运行用户析构器（PHP 语义）：
 *  - DTORED 标记保证每个对象最多析构一次
 *  - 析构期间的"引擎引用"防止析构器内释放自身导致 UAF；析构后 rc 仍 >1
 *    说明对象被复活（析构器里又存了一份），保持存活不再释放
 *  - GC sweep 期间不运行 PHP（快照遍历中跑用户代码会 UAF）：标记后挂入
 *    defer 列表，sweep 全部结束后再跑（php-src 同款两段式）
 *  - enum 单例不析构（PHP 枚举不可实例化，无析构语义） */
static bool g_in_gc;
#define DTR_DEFER_MAX 1024
static TObj *g_dtor_defer[DTR_DEFER_MAX];
static int g_ndefer;

static bool obj_has_dtor(const TObj *o) {
    return o->cls->kind != CK_ENUM && o->cls->m_dtor >= 0 &&
           o->cls->m_dtor < o->cls->n_methods;
}

/* sweep 结束后的 defer 析构（对象仍被引擎引用持有，不会 UAF） */
static void gc_run_deferred_dtors(void) {
    if (!g_ndefer) return;
    TObj *list[DTR_DEFER_MAX];
    int n = g_ndefer;
    memcpy(list, g_dtor_defer, sizeof(TObj *) * (size_t)n);
    g_ndefer = 0;
    g_in_gc = false;
    for (int i = 0; i < n; i++) {
        TObj *o = list[i];
        tphp_obj_run_dtor(o);
        o->h.rc--;                        /* 退掉引擎引用 */
        if (o->h.rc == 0)
            tobj_free(o);                 /* DTORED 已置位 → 直连释放路径 */
    }
}

void tobj_free(TObj *o) {
    if (obj_has_dtor(o) && !(o->h.flags & GC_FLAG_DTORED)) {
        o->h.flags |= GC_FLAG_DTORED;
        o->h.rc++;                        /* 引擎引用：析构期间保命 */
        if (g_in_gc) {
            if (g_ndefer < DTR_DEFER_MAX) g_dtor_defer[g_ndefer++] = o;
            return;                       /* 保持存活，sweep 后统一析构 */
        }
        tphp_obj_run_dtor(o);
        o->h.rc--;                        /* 退掉引擎引用 */
        /* rc==1 = 只剩"正在释放的这个引用"（tval_release 在 rc==1 时调用
         * 本函数，所有权已移交）——不是复活，应继续释放；rc>1 才是析构器
         * 里又被存了一份（复活），对象保留。写成 >0 会把正常路径全拦成
         * "复活"，属性永不释放、嵌套析构器不到场（2026-10-15 修）。 */
        if (o->h.rc > 1) return;          /* 被复活：不再释放 */
    }
    if (o->cls->native_dtor)
        o->cls->native_dtor(o);   /* M5.5 FFI\CData owned-buffer release */
    for (int i = 0; i < o->cls->n_props; i++)
        tval_release(&o->props[i]);
    tm_free(o->props);
    gc_unregister(&o->h);
    tm_free(o);
}

void tex_free(TExObj *x) {
    if (x->wrap) {                  /* P1-2: 用户异常类包装 */
        TVal w = { .u.p = x->wrap, .t = T_OBJ };
        tval_release(&w);
    }
    if (x->message)
        tval_release(&(TVal){ .u.p = x->message, .t = T_STR });
    if (x->previous)
        tval_release(&(TVal){ .u.p = x->previous, .t = T_OBJ });
    gc_unregister(&x->h);
    tm_free(x);
}

/* ---- garbage collection ----
 * php-src style rc-simulation over the full registry (no explicit roots
 * needed: any object whose rc exceeds the sum of internal references is
 * externally reachable, which also keeps transient C-stack values safe):
 *   1. gc_ref = rc for every object
 *   2. for every array, decrement gc_ref of each child it references
 *   3. objects with gc_ref > 0 are externally reachable: dfs-mark black
 *      through children
 *   4. everything still unmarked is only cycle-held: free it
 */
static void gc_child_release(TVal *v);

static void gc_arr_children(TArr *a, void (*fn)(TVal *)) {
    for (uint32_t i = 0; i < a->dused; i++)
        if (a->data[i].t != T_UNDEF) fn(&a->data[i]);
    for (uint32_t i = 0; i < a->bcap; i++)
        if (a->buckets[i].val_off != UINT32_MAX) fn(&a->buckets[i].key);
}

/* gc traversal of one object's children (arrays: elements+keys;
 * closures: captured values; exceptions: message + previous chain;
 * user objects: property slots) */
static void gc_children(GCHead *h, void (*fn)(TVal *)) {
    if (h->kind == GC_KIND_ARR) {
        gc_arr_children((TArr *)h, fn);
    } else if (h->kind == GC_KIND_CLO) {
        TClosure *c = (TClosure *)h;
        for (uint16_t i = 0; i < c->n_cap; i++)
            fn(&c->captures[i]);
    } else if (h->kind == GC_KIND_EX) {
        TExObj *x = (TExObj *)h;
        if (x->wrap)                 /* P1-2: 用户异常对象也参与 GC 遍历 */
            fn(&(TVal){ .u.p = x->wrap, .t = T_OBJ });
        if (x->message)
            fn(&(TVal){ .u.p = x->message, .t = T_STR });
        if (x->previous)
            fn(&(TVal){ .u.p = x->previous, .t = T_OBJ });
    } else if (h->kind == GC_KIND_OBJ) {
        TObj *o = (TObj *)h;
        for (int i = 0; i < o->cls->n_props; i++)
            fn(&o->props[i]);
    }
}

/* release one child during GC sweep: cycle members just lose a count */
static void gc_child_release(TVal *v) {
    if (v->t < T_STR || v->t > T_CLOSURE) return;
    GCHead *h = (GCHead *)v->u.p;
    if (h->flags & GC_FLAG_INTERNED) return;
    if (h->color == GC_COLOR_DEAD) { h->rc--; return; }
    if (h->rc > 1) { h->rc--; return; }
    /* rc == 1: last reference during a non-GC release path */
    switch (h->kind) {
    case GC_KIND_STR: tstr_free((TStr *)h); break;
    case GC_KIND_ARR: tarr_free((TArr *)h); break;
    case GC_KIND_CLO: tclo_free((TClosure *)h); break;
    case GC_KIND_EX:  tex_free((TExObj *)h); break;
    case GC_KIND_OBJ: tobj_free((TObj *)h); break;
    }
}

void tval_release(TVal *v) {
    if (v->t < T_STR || v->t > T_CLOSURE) return;
    GCHead *h = (GCHead *)v->u.p;
    if (h->flags & GC_FLAG_INTERNED) return;
    if (h->color == GC_COLOR_DEAD) { h->rc--; return; }   /* GC sweep path */
    if (h->rc > 1) { h->rc--; return; }
    switch (h->kind) {
    case GC_KIND_STR: tstr_free((TStr *)h); break;
    case GC_KIND_ARR: tarr_free((TArr *)h); break;
    case GC_KIND_CLO: tclo_free((TClosure *)h); break;
    case GC_KIND_EX:  tex_free((TExObj *)h); break;
    case GC_KIND_OBJ: tobj_free((TObj *)h); break;
    }
}

static void gc_mark(TVal *v) {
    if (v->t < T_STR || v->t > T_CLOSURE) return;
    GCHead *h = (GCHead *)v->u.p;
    if (h->color == GC_COLOR_BLACK || (h->flags & GC_FLAG_INTERNED)) return;
    h->color = GC_COLOR_BLACK;
    gc_children(h, gc_mark);
}

/* decrement one child's gref if it is in the registry (any refcounted kind;
 * interned strings never participate) */
static void gref_dec(GCHead **objs, int64_t *gref, size_t count, TVal *v) {
    if (v->t < T_STR || v->t > T_CLOSURE) return;
    GCHead *c = (GCHead *)v->u.p;
    if (c->flags & GC_FLAG_INTERNED) return;
    for (size_t j = 0; j < count; j++)
        if (objs[j] == c) { gref[j]--; return; }
}

int64_t gc_collect(void) {
    int64_t freed = 0;
    size_t count = 0;
    g_in_gc = true;
    for (GCHead *h = gc_list; h; h = h->gc_next) {
        h->color = GC_COLOR_UNMARKED;
        count++;
    }
    if (count == 0) { g_in_gc = false; return 0; }
    GCHead **objs = tm_malloc(sizeof(GCHead *) * count);
    int64_t *gref = tm_malloc(sizeof(int64_t) * count);
    size_t i = 0;
    for (GCHead *h = gc_list; h; h = h->gc_next) { objs[i] = h; gref[i] = h->rc; i++; }
    /* index registry pointers (small M1 scale: linear lookup) */
    for (size_t k = 0; k < count; k++) {
        GCHead *h = objs[k];
        if (h->kind == GC_KIND_ARR) {
            TArr *a = (TArr *)h;
            for (uint32_t d = 0; d < a->dused; d++)
                gref_dec(objs, gref, count, &a->data[d]);
            for (uint32_t b = 0; b < a->bcap; b++) {
                if (a->buckets[b].val_off == UINT32_MAX) continue;
                gref_dec(objs, gref, count, &a->buckets[b].key);
            }
        } else if (h->kind == GC_KIND_CLO) {
            TClosure *c = (TClosure *)h;
            for (uint16_t i = 0; i < c->n_cap; i++)
                gref_dec(objs, gref, count, &c->captures[i]);
        } else if (h->kind == GC_KIND_EX) {
            TExObj *x = (TExObj *)h;
            if (x->wrap)
                gref_dec(objs, gref, count, &(TVal){ .u.p = x->wrap, .t = T_OBJ });
            if (x->message)
                gref_dec(objs, gref, count, &(TVal){ .u.p = x->message, .t = T_STR });
            if (x->previous)
                gref_dec(objs, gref, count, &(TVal){ .u.p = x->previous, .t = T_OBJ });
        } else if (h->kind == GC_KIND_OBJ) {
            /* 2026-10-15：补对象属性边。此前 OBJ 完全不参与 gref 递减，
             * 任何含对象的环都因 gref>0 被误判可达——既不回收也不析构。 */
            TObj *o = (TObj *)h;
            for (int i = 0; i < o->cls->n_props; i++)
                gref_dec(objs, gref, count, &o->props[i]);
        }
    }
    /* 3. mark externally-reachable objects and everything under them */
    for (size_t k = 0; k < count; k++) {
        if (gref[k] > 0) {
            TVal probe = { .u.p = objs[k], .t = objs[k]->kind == GC_KIND_STR ? T_STR : T_ARR };
            gc_mark(&probe);
        }
    }
    /* 4. sweep: unmarked objects are cycle-only */
    for (size_t k = 0; k < count; k++) {
        GCHead *h = objs[k];
        if (h->color == GC_COLOR_BLACK || (h->flags & GC_FLAG_INTERNED)) continue;
        h->color = GC_COLOR_DEAD;
    }
    for (size_t k = 0; k < count; k++) {
        GCHead *h = objs[k];
        if (h->color != GC_COLOR_DEAD) continue;
        /* release children: cycle members only lose counts (color DEAD) */
        gc_children(h, gc_child_release);
        freed++;
    }
    for (size_t k = 0; k < count; k++) {
        GCHead *h = objs[k];
        if (h->color != GC_COLOR_DEAD) continue;
        /* OBJ/CLO/EX 一并释放：其子引用经 tval_release 的 DEAD 色协议
         * 安全递减（2026-10-15：此前只释放 STR/ARR，环中对象/闭包/异常
         * 全部泄漏）。带 __destruct 的对象在 tobj_free 内挂起 defer。 */
        switch (h->kind) {
        case GC_KIND_STR: tstr_free((TStr *)h); break;
        case GC_KIND_ARR: tarr_free((TArr *)h); break;
        case GC_KIND_CLO: tclo_free((TClosure *)h); break;
        case GC_KIND_EX:  tex_free((TExObj *)h); break;
        case GC_KIND_OBJ: tobj_free((TObj *)h); break;
        }
    }
    gc_run_deferred_dtors();
    tm_free(objs);
    tm_free(gref);
    return freed;
}
