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
#include <stdio.h>
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

/* interned 同串指针 / int 同值快路（tstr_eq 首行 a==b 同款）——IC 热路径
 * 省一次跨 TU call；不等再落 key_eq 全量比较，语义严格等价 */
#define KEY_PTR_EQ(bk, k) ((bk).t == (k).t && (bk).u.p == (k).u.p)

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
    if (a->bpending) tarr_sync_buckets(a);   /* packed append 惰性桶：查桶前先补齐
                                              * （守卫前置：非 pending 时免调用） */
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

/* norm 快路（批A kv 剖面：tarr_get 9.6 vs 手写 3.1，差值主项=跨 TU 的
 * tval_norm_key 调用层）：interned 非数字键（tstr_intern 时已判并打
 * GC_FLAG_NONNUM_KEY，flagged ⟹ interned ⟹ 无转换需求）直接跳过调用；
 * 其余（int/bool/null/float/未 intern/数字串）仍走完整 norm。 */
static inline bool key_norm_fast(TVal *k) {
    if (k->t == T_STR && (((TStr *)k->u.p)->h.flags & GC_FLAG_NONNUM_KEY))
        return true;
    return tval_norm_key(k);
}

bool tarr_set(TArr *a, TVal key, TVal val) {
    /* PHP parity（批4 剖面）：$m[$i]=$v 且 $i==next_idx 就是追加——
     * zend_hash nextFreeElement 快路同构。顺序 int 键此前走全探测
     * （kv-comp 剖面：+105ns vs append 地板，PHP 仅 +2ns）。packed
     * 不变量保证 key 必不存在（0..next_idx-1 已在），纯插入零探测。 */
    if (a->packed && key.t == T_INT && key.u.i == a->next_idx) {
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
    if (!key_norm_fast(&key)) return false;
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
    /* 单次探测（批2①）：find 同时取 free_slot——键存在→替换；不存在时
     * 仅在 grow/compact 改变桶布局后才重新探测（原先第二次 arr_find 无
     * 条件执行，miss 路径每插入多付一次 hash+probe）。 */
    uint32_t free_slot;
    ABucket *b = arr_find(a, key, &free_slot);
    if (b) {
        tval_release(&a->data[b->val_off]);
        a->data[b->val_off] = val;
        return true;
    }
    uint32_t bcap0 = a->bcap, dused0 = a->dused;
    arr_maybe_grow(a);
    /* too many holes from unsets: compact before appending */
    if (a->dused >= 8 && a->dused - a->n >= a->dused / 2)
        arr_compact(a);
    if (a->bcap != bcap0 || a->dused != dused0) {
        b = arr_find(a, key, &free_slot); /* 桶布局已变：重探 */
        if (b) {                          /* 防御：grow/compact 不产生新键 */
            tval_release(&a->data[b->val_off]);
            a->data[b->val_off] = val;
            return true;
        }
    }
    if (free_slot == UINT32_MAX) {
        arr_grow_buckets(a, a->bcap * 2);
        b = arr_find(a, key, &free_slot);
        if (b) {                          /* 防御：同上 */
            tval_release(&a->data[b->val_off]);
            a->data[b->val_off] = val;
            return true;
        }
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
    if (!key_norm_fast(&key)) return NULL;
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

/* ---- P0-16 批C：IC 链统计（定义前置：tarr_get_ic 热路径引用）---- */
DimIcStats tphp_ic_stats;
static bool ic_on, ic_on_init;

void tphp_ic_stats_dump(void) {
    DimIcStats *s = &tphp_ic_stats;
    if (!s->calls) return;
    double avg = s->fast ? (double)s->scan_total / (double)s->fast : 0.0;
    fprintf(stderr,
            "IC-STATS calls=%llu fast=%llu (%.1f%%) avg_scan=%.2f "
            "max_scan=%llu slow=%llu fills=%llu absent=%llu\n",
            (unsigned long long)s->calls,
            (unsigned long long)s->fast,
            100.0 * (double)s->fast / (double)s->calls,
            avg,
            (unsigned long long)s->scan_max,
            (unsigned long long)s->slow,
            (unsigned long long)s->fills,
            (unsigned long long)s->absent);
}

TVal *tarr_get_ic(TArr *a, TVal key, DimIC *ic) {
    /* packed int 键快路同 tarr_get（IC 无意义——IR packed 直取同款）。
     * 不计入统计：IC 链从不服务此形态。 */
    if (a->packed && key.t == T_INT) {
        int64_t i = key.u.i;
        return (i >= 0 && (uint32_t)i < a->n) ? &a->data[i] : NULL;
    }
    /* P0-16 批C：统计 gate（TPHP_IC_STATS=1；默认关 = 一个可预测分支） */
    if (!ic_on_init) {
        ic_on = getenv("TPHP_IC_STATS") != NULL;
        ic_on_init = true;
        if (ic_on) atexit(tphp_ic_stats_dump);
    }
    if (ic_on) tphp_ic_stats.calls++;
    /* 批C 两级快路（替代 8-case 线性链）：插桩实证——单态 site（tests/
     * bench 字面量形态）avg_scan=1.00，单槽 1 步最优；多态轮转 site
     * （bench-vs-php 变量键）avg_scan=4.48（理论 4.5），链的 O(K/2) 线
     * 性扫换 O(1) hash 直达（C 层微基准 14.0→2.2ns）。两级都锚定快照
     * 门（arr/bk/bpending），key_eq 全值比较，撕裂安全门不变。 */
    if (ic && !a->bpending && a == ic->arr && a->buckets == ic->bk) {
        /* ① 单槽（cases[0] = 最近命中桶；单态 site 一步命中） */
        ABucket *b0 = ic->cases[0];
        if (b0 != &ic->dead && b0->val_off != UINT32_MAX &&
            (KEY_PTR_EQ(b0->key, key) || key_eq(b0->key, key))) {
            if (ic_on) {
                tphp_ic_stats.fast++;
                tphp_ic_stats.scan_total++;
                if (tphp_ic_stats.scan_max < 1) tphp_ic_stats.scan_max = 1;
            }
            return &a->data[b0->val_off];
        }
        /* ② hash 直达（T_STR 键 hash 缓存于 TStr；限 4 次探测，超出落
         * 慢路 arr_find 兜底——语义与原链 miss 同路） */
        if (key.t == T_STR) {
            TStr *ks = (TStr *)key.u.p;
            uint32_t slot = (ks->hash ? ks->hash : tstr_hash(ks)) & a->hmask;
            for (int p = 0; p < 4; p++) {
                ABucket *b = &a->buckets[slot];
                if (b->key.t == T_NULL) break;   /* 探测链终止：absent */
                if (b->val_off != UINT32_MAX &&
                    (KEY_PTR_EQ(b->key, key) || key_eq(b->key, key))) {
                    ic->cases[0] = b;   /* 单槽回填（cases 序变化对 IR 链
                                         * 安全：全值比较 + 有效桶集合语义） */
                    if (ic_on) {
                        tphp_ic_stats.fast++;
                        tphp_ic_stats.scan_total += (uint64_t)(p + 2);
                        if (p + 2 > (int)tphp_ic_stats.scan_max)
                            tphp_ic_stats.scan_max = (uint64_t)(p + 2);
                    }
                    return &a->data[b->val_off];
                }
                slot = (slot + 1) & a->hmask;
            }
        }
        if (ic_on && tphp_ic_stats.scan_max < DIM_IC_CASES)
            tphp_ic_stats.scan_max = DIM_IC_CASES;
    }
    ABucket *b = arr_find(a, key, NULL);
    if (!b) {
        if (ic_on) tphp_ic_stats.absent++;
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
        if (ic_on) tphp_ic_stats.fills++;
    }
    if (ic_on) tphp_ic_stats.slow++;
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
    if (!key_norm_fast(&key)) return false;
    if (a->packed && key.t == T_INT) {   /* 快路同 tarr_get，免无谓 sync */
        int64_t i = key.u.i;
        return i >= 0 && (uint32_t)i < a->n;
    }
    return arr_find(a, key, NULL) != NULL;
}

bool tarr_unset(TArr *a, TVal key) {
    if (!key_norm_fast(&key)) return false;
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
