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
    return tstr_hash((TStr *)key.u.p);
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

bool tval_norm_key(TVal *key); /* value.c */

bool tarr_set(TArr *a, TVal key, TVal val) {
    if (!tval_norm_key(&key)) return false;
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
    return tarr_set(a, TV_INT(a->next_idx), val);
}

TVal *tarr_get(TArr *a, TVal key) {
    if (!tval_norm_key(&key)) return NULL;
    ABucket *b = arr_find(a, key, NULL);
    return b ? &a->data[b->val_off] : NULL;
}

bool tarr_has(TArr *a, TVal key) {
    if (!tval_norm_key(&key)) return false;
    return arr_find(a, key, NULL) != NULL;
}

bool tarr_unset(TArr *a, TVal key) {
    if (!tval_norm_key(&key)) return false;
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
    if (a->dcap) {
        b->data = tm_malloc(sizeof(TVal) * a->dcap);
        memcpy(b->data, a->data, sizeof(TVal) * a->dcap);
        for (uint32_t i = 0; i < a->dused; i++) {
            if (b->data[i].t == T_UNDEF) continue;   /* free slot */
            tval_addref(b->data[i]);
        }
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
