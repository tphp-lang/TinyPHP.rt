/*
 * TinyPHP.rt - a small PHP runtime
 * array.c - insertion-ordered hash table with PHP array key semantics
 *
 * keys are normalized at every boundary (int/bool/null/float -> int,
 * canonical decimal strings -> int); values live in data[] in insertion
 * order so foreach iterates without sorting.
 */
#include "value.h"
#include <stdlib.h>
#include <string.h>

TArr *tarr_alloc_raw(void); /* mem.c */
TArr *tarr_new(void) { return tarr_alloc_raw(); }

static uint32_t key_hash(TVal key) {
    if (key.t == T_INT) {
        uint64_t x = (uint64_t)key.u.i;
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
        return (uint32_t)(x | 1);
    }
    return TSTR_HASH((TStr *)key.u.p);
}

static bool key_eq(TVal a, TVal b) {
    if (a.t != b.t) return false;
    if (a.t == T_INT) return a.u.i == b.u.i;
    return tstr_eq((TStr *)a.u.p, (TStr *)b.u.p);
}

static void arr_grow_buckets(TArr *a, uint32_t new_bcap) {
    ABucket *nb = tm_malloc(sizeof(ABucket) * new_bcap);
    for (uint32_t i = 0; i < new_bcap; i++) {
        nb[i].key.t = T_NULL;
        nb[i].val_off = UINT32_MAX;
    }
    uint32_t mask = new_bcap - 1;
    if (a->buckets) {
        for (uint32_t i = 0; i < a->bcap; i++) {
            ABucket *b = &a->buckets[i];
            if (b->val_off == UINT32_MAX || b->key.t == T_NULL) continue;
            uint32_t slot = key_hash(b->key) & mask;
            while (nb[slot].val_off != UINT32_MAX) slot = (slot + 1) & mask;
            nb[slot] = *b;
        }
        tm_free(a->buckets);
    }
    a->buckets = nb;
    a->bcap = new_bcap;
    a->hmask = mask;
}

/* find the bucket for key; returns NULL when absent.
 * *free_slot_out (optional) receives the first reusable slot (tombstone or
 * the empty slot that stopped the probe). */
static ABucket *arr_find(TArr *a, TVal key, uint32_t *free_slot_out) {
    tarr_sync_buckets(a);   /* packed append 惰性桶：查桶前先补齐 */
    if (!a->buckets) return NULL;
    uint32_t slot = key_hash(key) & a->hmask;
    uint32_t free_slot = UINT32_MAX;
    for (uint32_t probes = 0; probes < a->bcap; probes++) {
        ABucket *b = &a->buckets[slot];
        if (b->key.t == T_NULL) {           /* never used: end of probe chain */
            if (free_slot_out) {
                if (free_slot == UINT32_MAX) free_slot = slot;
                *free_slot_out = free_slot;
            }
            return NULL;
        }
        if (b->val_off == UINT32_MAX) {     /* tombstone: remember, keep probing */
            if (free_slot == UINT32_MAX) free_slot = slot;
        } else if (key_eq(b->key, key)) {
            if (free_slot_out) *free_slot_out = free_slot;
            return b;
        }
        slot = (slot + 1) & a->hmask;
    }
    if (free_slot_out) *free_slot_out = free_slot;
    return NULL;
}

/* new values always append (PHP keeps insertion order; unset holes stay
 * until compaction), matching zend_hash's nNumUsed behaviour */
static uint32_t arr_take_data_slot(TArr *a) {
    if (a->dused == a->dcap) {
        a->dcap = a->dcap ? a->dcap * 2 : 8;
        a->data = tm_realloc(a->data, sizeof(TVal) * a->dcap);
    }
    return a->dused++;
}

/* squeeze out T_UNDEF holes, keeping data[] in insertion order, and
 * rebuild the bucket table for the new slot positions */
static void arr_compact(TArr *a) {
    typedef struct { TVal key; uint32_t di; } Pair;
    Pair *pairs = tm_malloc(sizeof(Pair) * (size_t)(a->n ? a->n : 1));
    uint32_t np = 0;
    for (uint32_t b = 0; b < a->bcap; b++) {
        ABucket *bk = &a->buckets[b];
        if (bk->key.t != T_NULL && bk->val_off != UINT32_MAX) {
            pairs[np].key = bk->key;
            pairs[np].di = bk->val_off;
            np++;
        }
    }
    /* insertion sort by physical slot (data order) */
    for (uint32_t i = 1; i < np; i++) {
        Pair pv = pairs[i];
        uint32_t j = i;
        while (j > 0 && pairs[j - 1].di > pv.di) { pairs[j] = pairs[j - 1]; j--; }
        pairs[j] = pv;
    }
    for (uint32_t i = 0; i < np; i++)
        if (pairs[i].di != i) a->data[i] = a->data[pairs[i].di];
    a->dused = np;
    a->free_head = UINT32_MAX;
    for (uint32_t b = 0; b < a->bcap; b++) {
        a->buckets[b].key.t = T_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    for (uint32_t i = 0; i < np; i++) {
        uint32_t slot = key_hash(pairs[i].key) & a->hmask;
        while (a->buckets[slot].val_off != UINT32_MAX) slot = (slot + 1) & a->hmask;
        a->buckets[slot].key = pairs[i].key;
        a->buckets[slot].val_off = i;
    }
    tm_free(pairs);
}

static void arr_maybe_grow(TArr *a) {
    if (!a->buckets || a->n * 4 >= a->bcap * 3)
        arr_grow_buckets(a, a->bcap ? a->bcap * 2 : 16);
}

/* 惰性桶同步（L5）：把 data[0..n) 全量重建进 buckets。bpending>0 时全部
 * 键都是 int 0..n-1（不变量），旧桶里的 int 键无需 release；重建后 packed
 * 布局不变（packed 保持，tarr_get 快路仍有效）。O(n)，只在有人查桶时付。 */
void tarr_sync_buckets(TArr *a) {
    if (a->bpending == 0) return;
    a->bpending = 0;
    uint32_t bcap = a->bcap ? a->bcap : 16;
    while (a->n * 4 >= bcap * 3) bcap *= 2;   /* 载荷 < 0.75，防建完即-grow */
    if (bcap != a->bcap || !a->buckets) {
        if (a->buckets) tm_free(a->buckets);
        a->buckets = tm_malloc(sizeof(ABucket) * (size_t)bcap);
        a->bcap = bcap;
        a->hmask = bcap - 1;
    }
    for (uint32_t i = 0; i < a->bcap; i++) {
        a->buckets[i].key.t = T_NULL;
        a->buckets[i].val_off = UINT32_MAX;
    }
    for (uint32_t i = 0; i < a->n; i++) {     /* packed：data[i] 即键 i */
        TVal key = TV_INT((int64_t)i);
        uint32_t slot = key_hash(key) & a->hmask;
        while (a->buckets[slot].val_off != UINT32_MAX)
            slot = (slot + 1) & a->hmask;
        a->buckets[slot].key = key;
        a->buckets[slot].val_off = i;
    }
}

bool tval_norm_key(TVal *key); /* value.c */

bool tarr_set(TArr *a, TVal key, TVal val) {
    if (!tval_norm_key(&key)) return false;
    /* L3 键 intern（PHP zend hash 同款）：查找键比较 memcmp → 指针相等；
     * interned 串不 refcount、addref/release 为 no-op。永久驻留（CLI 单
     * 进程 ≈ PHP request 级 intern 生命周期）。 */
    if (key.t == T_STR) {
        TStr *ks = (TStr *)key.u.p;
        if (!(ks->h.flags & GC_FLAG_INTERNED))
            key = TV_STR(tstr_intern(ks->c, ks->len));
    }
    /* packed 维护（L2）：仅"连续 append int 键"保持；其余写入打破。
     * 不变量：packed=true ⇒ data[i] 恒为键 i（i=0..n-1）无洞。 */
    if (!a->packed || key.t != T_INT || key.u.i != a->next_idx)
        a->packed = false;
    ABucket *b = arr_find(a, key, NULL);
    if (b) {
        tval_release(&a->data[b->val_off]);
        a->data[b->val_off] = val;
        return true;
    }
    arr_maybe_grow(a);
    /* too many holes from unsets: compact before appending */
    if (a->dused >= 8 && a->dused - a->n >= a->dused / 2)
        arr_compact(a);
    uint32_t free_slot;
    b = arr_find(a, key, &free_slot); /* re-find after possible rehash */
    if (!b) {
        if (free_slot == UINT32_MAX) {
            arr_grow_buckets(a, a->bcap * 2);
            b = arr_find(a, key, &free_slot);
        }
        uint32_t slot = free_slot;
        if (slot == UINT32_MAX) {
            /* table full of tombstones: rebuild and retry */
            arr_grow_buckets(a, a->bcap);
            b = arr_find(a, key, &slot);
            if (b) {
                tval_release(&a->data[b->val_off]);
                a->data[b->val_off] = val;
                return true;
            }
        }
        b = &a->buckets[slot];
        b->key = key;
        tval_addref(key);
        uint32_t di = arr_take_data_slot(a);
        b->val_off = di;
        a->data[di] = val;
        a->n++;
    } else {
        tval_release(&a->data[b->val_off]);
        a->data[b->val_off] = val;
    }
    if (key.t == T_INT && key.u.i >= a->next_idx) a->next_idx = key.u.i + 1;
    return true;
}

bool tarr_append(TArr *a, TVal val) {
    /* packed 快路（L5）：直接 data[n]=val，桶惰性补齐（bpending）——
     * 跳过 norm/find/grow/双查/桶写。非 packed 走全套 tarr_set。 */
    if (a->packed) {
        if (a->dused == a->dcap) {
            a->dcap = a->dcap ? a->dcap * 2 : 8;
            a->data = tm_realloc(a->data, sizeof(TVal) * a->dcap);
        }
        a->data[a->dused++] = val;
        a->n++;
        a->next_idx++;
        a->bpending++;
        return true;
    }
    return tarr_set(a, TV_INT(a->next_idx), val);
}

TVal *tarr_get(TArr *a, TVal key) {
    if (!tval_norm_key(&key)) return NULL;
    /* packed 快路径（L2）：int 键直取 data[i]，跳过哈希探测 */
    if (a->packed && key.t == T_INT) {
        int64_t i = key.u.i;
        if (i >= 0 && (uint32_t)i < a->n)
            return &a->data[i];
        return NULL;
    }
    ABucket *b = arr_find(a, key, NULL);
    return b ? &a->data[b->val_off] : NULL;
}

TVal *tarr_get_ic(TArr *a, TVal key, DimIC *ic) {
    /* packed int 键快路同 tarr_get（IC 无意义——IR packed 直取同款）。 */
    if (a->packed && key.t == T_INT) {
        int64_t i = key.u.i;
        return (i >= 0 && (uint32_t)i < a->n) ? &a->data[i] : NULL;
    }
    /* IC 多 case 快路径（v3）：共享 arr/bk 层 + cases[] 按序 key 比较——
     * 全部锚定**当前 a** 的真实状态，任何非原子竞态写最坏 miss 不会错
     * 命中（写序铁律见 value.h：cases 先于共享 arr/bk）。 */
    if (ic && !a->bpending && a == ic->arr && a->buckets == ic->bk) {
        for (int i = 0; i < DIM_IC_CASES; i++) {
            ABucket *b = ic->cases[i];
            if (b != &ic->dead && b->val_off != UINT32_MAX &&
                key_eq(b->key, key))
                return &a->data[b->val_off];
        }
    }
    ABucket *b = arr_find(a, key, NULL);
    if (!b) {
        if (ic) ic->arr = NULL;   /* absent：防陈旧快照下轮假 miss 循环 */
        return NULL;
    }
    if (ic && !a->bpending) {
        if (ic->arr != a || ic->bk != a->buckets) {
            /* 数组/桶表变化：先重置 cases 为哨兵（写序：cases 先于
             * 共享层），再刷共享快照——读侧 L0/L1 是安全门 */
            for (int i = 0; i < DIM_IC_CASES; i++) ic->cases[i] = &ic->dead;
            ic->next = 0;
            ic->arr = a;
            ic->bk = a->buckets;
        }
        ic->cases[ic->next++ & (DIM_IC_CASES - 1)] = b;   /* FIFO 装填 */
    }
    return &a->data[b->val_off];
}

/* IC 分配后的统一初始化（emit 懒分配与解释器懒分配共用）：哨兵恒非
 * NULL 是 IR 链"每 case 单 bool 线性递归"的前提 */
void tphp_dim_ic_init(DimIC *ic) {
    ic->arr = NULL;
    ic->bk = NULL;
    ic->misses = 0;
    ic->next = 0;
    ic->dead.key = TV_NULL;
    ic->dead.val_off = UINT32_MAX;
    for (int i = 0; i < DIM_IC_CASES; i++)
        ic->cases[i] = &ic->dead;
}

bool tarr_has(TArr *a, TVal key) {
    if (!tval_norm_key(&key)) return false;
    if (a->packed && key.t == T_INT) {   /* 快路同 tarr_get，免无谓 sync */
        int64_t i = key.u.i;
        return i >= 0 && (uint32_t)i < a->n;
    }
    return arr_find(a, key, NULL) != NULL;
}

bool tarr_unset(TArr *a, TVal key) {
    if (!tval_norm_key(&key)) return false;
    a->packed = false;   /* 洞出现，data 不再连续 */
    ABucket *b = arr_find(a, key, NULL);
    if (!b) return false;
    uint32_t di = b->val_off;
    tval_release(&a->data[di]);
    a->data[di].t = T_UNDEF;   /* hole; physical order is preserved */
    b->val_off = UINT32_MAX;   /* tombstone; key stays for probe chains */
    a->n--;
    return true;
}

TArr *tarr_cow(TArr *a) {
    if (a->h.rc <= 1) return a;
    TArr *b = tarr_alloc_raw();
    b->n = a->n;
    b->next_idx = a->next_idx;
    b->free_head = a->free_head;
    b->dcap = a->dcap;
    b->dused = a->dused;
    b->packed = a->packed;   /* L2：布局不变时快路径同样成立 */
    b->bpending = a->bpending;   /* 桶同样落后，复制方自行 sync */
    if (a->dcap) {
        b->data = tm_malloc(sizeof(TVal) * a->dcap);
        /* only copy the initialized prefix; zero the tail (memcpy of
         * uninitialized bytes is UB and crashes under some clang/OS) */
        memset(b->data, 0, sizeof(TVal) * a->dcap);
        if (a->dused)
            memcpy(b->data, a->data, sizeof(TVal) * a->dused);
        for (uint32_t i = 0; i < a->dused; i++) {
            if (b->data[i].t == T_UNDEF) continue;   /* free slot */
            tval_addref(b->data[i]);
        }
        /* mark tail as T_UNDEF so dump/GC never see zeroed-as-scalar */
        for (uint32_t i = a->dused; i < a->dcap; i++)
            b->data[i].t = T_UNDEF;
    }
    if (a->buckets) {
        b->bcap = a->bcap;
        b->hmask = a->hmask;
        b->buckets = tm_malloc(sizeof(ABucket) * a->bcap);
        memcpy(b->buckets, a->buckets, sizeof(ABucket) * a->bcap);
        for (uint32_t i = 0; i < a->bcap; i++) {
            ABucket *bk = &b->buckets[i];
            if (bk->key.t == T_STR && bk->val_off != UINT32_MAX)
                tval_addref(bk->key);
        }
    }
    return b;  /* rc of `a` untouched; caller swaps its slot */
}
