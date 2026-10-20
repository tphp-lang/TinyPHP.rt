/*
 * TinyPHP.rt - serialize.c - 跨线程值克隆 + serialize/unserialize（M5）
 *
 * 自有紧凑二进制格式（与 PHP serialize 文本不互通——与 PHP 交换数据
 * 仍用 JSON，见 PLAN/02 §4）：
 *
 *   'N'                          null
 *   'B' + u8                     bool
 *   'I' + i64 LE                 int
 *   'D' + f64 LE                 float
 *   'S' + u32 len + bytes        string
 *   'A' + u32 n + n × (key,val)  array（键复用值编码；按 data[] 插入序）
 *   'O' + S(cls) + (state)       object（state = __cloneState() 的数组）
 *   'E' + S(cls) + S(case)       enum case（反序列化取回该线程单例）
 *   'R' + u32 idx                backref（第 idx 个已写的复合值；DAG 去重）
 *
 * 循环引用检测：写复合值前登记 IN_PROGRESS，递归再遇到即抛 Error
 * （PLAN/02 §3：循环引用检测到即报错）。backref 只做 DAG 去重。
 *
 * 对象协议（vm.c）：__cloneState(): array 导出；恢复优先
 * __restoreCloneState(array)，否则 string 键逐一同名属性槽直填。
 * closure / 资源句柄：抛 NotCloneableError（PLAN/02 §3）。
 *
 * 所有权：serialize 返回 +1 TStr；unserialize 返回 +1 TVal。
 */
#include "value.h"
#include "class.h"
#include "opcode.h"
#include "tphp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- 小工具 ---- */

static bool grow(char **buf, size_t *cap, size_t need) {
    if (need <= *cap) return true;
    size_t nc = *cap ? *cap : 64;
    while (nc < need) nc *= 2;
    char *nb = tm_realloc(*buf, nc);
    if (!nb) return false;
    *buf = nb; *cap = nc;
    return true;
}

static void put_bytes(char **buf, size_t *len, size_t *cap,
                      const void *d, size_t n) {
    if (!grow(buf, cap, *len + n)) return;
    memcpy(*buf + *len, d, n);
    *len += n;
}

static void put_u8(char **b, size_t *l, size_t *c, uint8_t v) {
    put_bytes(b, l, c, &v, 1);
}

static void put_u32(char **b, size_t *l, size_t *c, uint32_t v) {
    put_bytes(b, l, c, &v, 4);      /* 小端（x86 目标，直接内存序） */
}

static void put_i64(char **b, size_t *l, size_t *c, int64_t v) {
    put_bytes(b, l, c, &v, 8);
}

static void put_f64(char **b, size_t *l, size_t *c, double v) {
    put_bytes(b, l, c, &v, 8);
}

/* ---- writer ---- */

typedef struct {
    VM *vm;
    char *buf; size_t len, cap;
    /* 复合值登记（DAG 去重 + 循环检测） */
    GCHead **seen; uint32_t *seen_idx; uint8_t *seen_state; /* 0=done 1=progress */
    uint32_t n_seen, cap_seen;
    uint32_t next_idx;
} SW;

static void sw_put(SW *w, const void *d, size_t n) {
    put_bytes(&w->buf, &w->len, &w->cap, d, n);
}

static void sw_put_str(SW *w, const char *s, uint32_t len) {
    put_u8(&w->buf, &w->len, &w->cap, 'S');
    put_u32(&w->buf, &w->len, &w->cap, len);
    sw_put(w, s, len);
}

static void sw_put_cstr(SW *w, const char *s) {
    sw_put_str(w, s, (uint32_t)strlen(s));
}

/* 复合值进入：-1 = 新值（先写内容后补完成），>=0 = backref 已有 */
static int sw_seen_enter(SW *w, GCHead *h) {
    for (uint32_t i = 0; i < w->n_seen; i++) {
        if (w->seen[i] == h) {
            if (w->seen_state[i] == 1)
                tphp_raise("Error",
                           "serialize(): circular reference detected");
            return (int)w->seen_idx[i];
        }
    }
    if (w->n_seen == w->cap_seen) {
        w->cap_seen = w->cap_seen ? w->cap_seen * 2 : 16;
        w->seen = tm_realloc(w->seen, sizeof(GCHead *) * w->cap_seen);
        w->seen_idx = tm_realloc(w->seen_idx, sizeof(uint32_t) * w->cap_seen);
        w->seen_state = tm_realloc(w->seen_state, w->cap_seen);
    }
    w->seen[w->n_seen] = h;
    w->seen_idx[w->n_seen] = w->next_idx++;  /* 预留槽位号（进入序 = reader
                                                遇到 'A'/'O' 的登记序） */
    w->seen_state[w->n_seen] = 1;
    w->n_seen++;
    return -1;
}

static void sw_seen_done(SW *w, GCHead *h) {
    for (uint32_t i = 0; i < w->n_seen; i++)
        if (w->seen[i] == h) { w->seen_state[i] = 0; return; }
}

static void sw_val(SW *w, TVal v);

static void sw_array(SW *w, TArr *a) {
    int ref = sw_seen_enter(w, &a->h);
    if (ref >= 0) {
        put_u8(&w->buf, &w->len, &w->cap, 'R');
        put_u32(&w->buf, &w->len, &w->cap, (uint32_t)ref);
        return;
    }
    put_u8(&w->buf, &w->len, &w->cap, 'A');
    put_u32(&w->buf, &w->len, &w->cap, a->n);
    /* data[] 插入序；off→key 逆映射（与 tval_clone_deep 同法） */
    TVal *keys = tm_malloc(sizeof(TVal) * (size_t)(a->dused ? a->dused : 1));
    for (uint32_t i = 0; i < a->dused; i++) keys[i].t = T_UNDEF;
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX &&
            a->buckets[b].val_off < a->dused)
            keys[a->buckets[b].val_off] = a->buckets[b].key;
    }
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF || keys[i].t == T_UNDEF) continue;
        sw_val(w, keys[i]);
        sw_val(w, a->data[i]);
    }
    tm_free(keys);
    sw_seen_done(w, &a->h);
}

static void sw_object(SW *w, TObj *o) {
    int ref = sw_seen_enter(w, &o->h);
    if (ref >= 0) {
        put_u8(&w->buf, &w->len, &w->cap, 'R');
        put_u32(&w->buf, &w->len, &w->cap, (uint32_t)ref);
        return;
    }
    if (o->cls->kind == CK_ENUM) {
        /* enum case：类名 + case 名，反序列化取回单例 */
        int ci = tphp_enum_case_find(o->cls, o);
        if (ci >= 0) {
            put_u8(&w->buf, &w->len, &w->cap, 'E');
            sw_put_cstr(w, tstr_val(o->cls->name));
            sw_put_cstr(w, tstr_val(o->cls->cases[ci].name));
            sw_seen_done(w, &o->h);
            return;
        }
    }
    TVal state;
    if (!tphp_vm_clone_state(w->vm, o, &state))
        tphp_raise("NotCloneableError",
                   "Serialization of class %s is not allowed: __cloneState() "
                   "is not implemented", tstr_val(o->cls->name));
    put_u8(&w->buf, &w->len, &w->cap, 'O');
    sw_put_cstr(w, tstr_val(o->cls->name));
    sw_val(w, state);            /* 状态数组（own：转移到流） */
    tval_release(&state);
    sw_seen_done(w, &o->h);
}

static void sw_val(SW *w, TVal v) {
    switch (v.t) {
    case T_NULL:  put_u8(&w->buf, &w->len, &w->cap, 'N'); break;
    case T_FALSE: put_u8(&w->buf, &w->len, &w->cap, 'B');
                  put_u8(&w->buf, &w->len, &w->cap, 0); break;
    case T_TRUE:  put_u8(&w->buf, &w->len, &w->cap, 'B');
                  put_u8(&w->buf, &w->len, &w->cap, 1); break;
    case T_INT:   put_u8(&w->buf, &w->len, &w->cap, 'I');
                  put_i64(&w->buf, &w->len, &w->cap, v.u.i); break;
    case T_FLOAT: put_u8(&w->buf, &w->len, &w->cap, 'D');
                  put_f64(&w->buf, &w->len, &w->cap, v.u.d); break;
    case T_STR: {
        TStr *s = (TStr *)v.u.p;
        sw_put_str(w, tstr_val(s), s->len);
        break;
    }
    case T_ARR:   sw_array(w, (TArr *)v.u.p); break;
    case T_OBJ:   sw_object(w, (TObj *)v.u.p); break;
    default:
        tphp_raise("NotCloneableError",
                   "Serialization of this value type is not supported");
    }
}

/* ---- reader ---- */

typedef struct {
    VM *vm;
    const char *d; size_t n, pos;
    TVal *refs; uint32_t n_refs, cap_refs;   /* idx → 值（持有） */
    bool ok;
} SR;

static bool sr_need(SR *r, size_t n) {
    if (!r->ok || r->pos + n > r->n) { r->ok = false; return false; }
    return true;
}

static uint8_t sr_u8(SR *r) {
    if (!sr_need(r, 1)) return 0;
    return (uint8_t)r->d[r->pos++];
}

static uint32_t sr_u32(SR *r) {
    if (!sr_need(r, 4)) return 0;
    uint32_t v;
    memcpy(&v, r->d + r->pos, 4);
    r->pos += 4;
    return v;
}

static int64_t sr_i64(SR *r) {
    if (!sr_need(r, 8)) return 0;
    int64_t v;
    memcpy(&v, r->d + r->pos, 8);
    r->pos += 8;
    return v;
}

static double sr_f64(SR *r) {
    if (!sr_need(r, 8)) return 0;
    double v;
    memcpy(&v, r->d + r->pos, 8);
    r->pos += 8;
    return v;
}

static TStr *sr_str(SR *r) {
    uint32_t len = sr_u32(r);
    if (!sr_need(r, len)) return NULL;
    TStr *s = tstr_new(r->d + r->pos, len);
    r->pos += len;
    return s;
}

static void sr_ref_add(SR *r, TVal v) {
    if (r->n_refs == r->cap_refs) {
        r->cap_refs = r->cap_refs ? r->cap_refs * 2 : 16;
        r->refs = tm_realloc(r->refs, sizeof(TVal) * r->cap_refs);
    }
    tval_addref(v);
    r->refs[r->n_refs++] = v;
}

static TVal sr_val(SR *r) {
    if (!r->ok) return TV_NULL;
    uint8_t tag = sr_u8(r);
    switch (tag) {
    case 'N': return TV_NULL;
    case 'B': return sr_u8(r) ? TV_TRUE : TV_FALSE;
    case 'I': return TV_INT(sr_i64(r));
    case 'D': return TV_FLOAT(sr_f64(r));
    case 'S': {
        TStr *s = sr_str(r);
        if (!s) { r->ok = false; return TV_NULL; }
        return TV_STR(s);
    }
    case 'R': {
        uint32_t idx = sr_u32(r);
        if (idx >= r->n_refs) { r->ok = false; return TV_NULL; }
        TVal v = r->refs[idx];
        tval_addref(v);
        return v;
    }
    case 'A': {
        uint32_t n = sr_u32(r);
        TArr *a = tarr_new();
        TVal av = TV_ARR(a);
        sr_ref_add(r, av);               /* backref 登记（自引用值到这） */
        for (uint32_t i = 0; i < n && r->ok; i++) {
            TVal key = sr_val(r);
            TVal val = sr_val(r);
            if (!r->ok) { tval_release(&key); tval_release(&val); break; }
            tarr_set(a, key, val);       /* val 转移；key 由 set addref */
            if (key.t == T_STR &&
                !(((TStr *)key.u.p)->h.flags & GC_FLAG_INTERNED))
                tval_release(&key);      /* 堆键抵消 */
        }
        return av;
    }
    case 'O': {
        /* 类名以完整 'S' 记录编码（与 writer 对称） */
        TVal clsv = sr_val(r);
        if (!r->ok || clsv.t != T_STR) { r->ok = false; return TV_NULL; }
        TStr *cls = (TStr *)clsv.u.p;
        /* registry keys on lowercased FQCN — fold the stored spelling */
        char lc[256];
        {
            const char *s = tstr_val(cls);
            size_t k = 0;
            for (; s[k] && k < sizeof lc - 1; k++)
                lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
            lc[k] = 0;
        }
        TClass *c = class_resolve_lc2(lc, tstr_val(cls));
        /* 类名大小写不敏感：先精确 lc，再按 PHP 规则已由 lookup 处理 */
        if (!c) {
            r->ok = false;
            tval_release(&clsv);
            return TV_NULL;
        }
        tval_release(&clsv);          /* 类名只用于查找 */
        TObj *o = tphp_obj_alloc(c);
        TVal ov = TV_OBJ(o);
        sr_ref_add(r, ov);
        TVal state = sr_val(r);
        if (!r->ok) { tval_release(&state); return ov; }
        tphp_vm_restore_clone_state(r->vm, o, state);
        tval_release(&state);
        return ov;
    }
    case 'E': {
        /* 类名/case 名均为完整 'S' 记录（与 writer 对称） */
        TVal clsv = sr_val(r);
        TVal casv = sr_val(r);
        if (!r->ok || clsv.t != T_STR || casv.t != T_STR) {
            r->ok = false;
            tval_release(&clsv);
            tval_release(&casv);
            return TV_NULL;
        }
        TStr *cls = (TStr *)clsv.u.p;
        TStr *cas = (TStr *)casv.u.p;
        char lc[256];
        {
            const char *s = tstr_val(cls);
            size_t k = 0;
            for (; s[k] && k < sizeof lc - 1; k++)
                lc[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] + 32) : s[k];
            lc[k] = 0;
        }
        TClass *c = class_resolve_lc2(lc, tstr_val(cls));
        TVal out = TV_NULL;
        if (c && c->kind == CK_ENUM) {
            for (int i = 0; i < c->n_cases; i++) {
                if (tstr_case_eq(tstr_val(c->cases[i].name), tstr_val(cas))) {
                    out = TV_OBJ(tphp_enum_case_obj(c, i));
                    break;
                }
            }
        }
        tval_release(&clsv);
        tval_release(&casv);
        if (out.t == T_NULL) r->ok = false;
        return out;
    }
    default:
        r->ok = false;
        return TV_NULL;
    }
}

/* ---- 公开入口 ---- */

/* 内存内深拷（M5-a：类表克隆播种 / RECV 数组默认值）：
 * 标量直传；INTERNED 串只读共享；堆串/数组递归拷贝（编译期常量区
 * 无别名/循环，无需 backref） */
TVal tval_clone_deep(TVal v) {
    if (v.t < T_STR) return v;                    /* null/bool/int/float */
    if (v.t == T_STR) {
        TStr *s = (TStr *)v.u.p;
        if (s->h.flags & GC_FLAG_INTERNED) return v;   /* 只读共享 */
        TStr *n = tstr_new(tstr_val(s), s->len);
        return TV_STR(n);
    }
    if (v.t == T_ARR) {
        TArr *a = (TArr *)v.u.p;
        TArr *n = tarr_new();
        /* data[] 物理序 = 插入序；off→key 逆映射一次扫描（O(n)） */
        TVal *keys = tm_malloc(sizeof(TVal) * (size_t)(a->dused ? a->dused : 1));
        for (uint32_t i = 0; i < a->dused; i++) keys[i].t = T_UNDEF;
        for (uint32_t b = 0; b < a->bcap; b++) {
            if (a->buckets[b].val_off != UINT32_MAX &&
                a->buckets[b].val_off < a->dused)
                keys[a->buckets[b].val_off] = a->buckets[b].key;
        }
        for (uint32_t i = 0; i < a->dused; i++) {
            if (a->data[i].t == T_UNDEF || keys[i].t == T_UNDEF) continue;
            TVal val = tval_clone_deep(a->data[i]);
            TVal key = keys[i];
            if (key.t == T_STR && !(((TStr *)key.u.p)->h.flags & GC_FLAG_INTERNED))
                key = tval_clone_deep(key);           /* 堆键：拷贝 */
            else
                tval_addref(key);                     /* interned/int 键 */
            tarr_set(n, key, val);                    /* val 转移 */
            if (key.t == T_STR &&
                !(((TStr *)key.u.p)->h.flags & GC_FLAG_INTERNED))
                tval_release(&key);                   /* 抵消 set 的 addref */
        }
        tm_free(keys);
        return TV_ARR(n);
    }
    return v;   /* T_OBJ/T_CLOSURE/T_ENUM_CASE：常量区不出现（对象克隆走
                   上面的字节流路径） */
}

/* serialize(v)：+1 TStr；不可克隆值直接抛 NotCloneableError/Error */
TStr *tphp_serialize_val(VM *vm, TVal v) {
    SW w;
    memset(&w, 0, sizeof w);
    w.vm = vm;
    sw_val(&w, v);
    TStr *s = tstr_new(w.buf ? w.buf : "", w.len);
    tm_free(w.buf);
    tm_free(w.seen);
    tm_free(w.seen_idx);
    tm_free(w.seen_state);
    return s;
}

/* unserialize(s)：成功 *out = +1 值返回 true；格式错返回 false（调用方
 * warning + false，对齐 PHP） */
bool tphp_unserialize_val(VM *vm, const char *d, size_t n, TVal *out) {
    SR r;
    memset(&r, 0, sizeof r);
    r.vm = vm;
    r.d = d; r.n = n; r.ok = true;
    TVal v = sr_val(&r);
    if (r.ok && r.pos == r.n) {
        for (uint32_t i = 0; i < r.n_refs; i++) tval_release(&r.refs[i]);
        tm_free(r.refs);
        *out = v;
        return true;
    }
    tval_release(&v);
    for (uint32_t i = 0; i < r.n_refs; i++) tval_release(&r.refs[i]);
    tm_free(r.refs);
    return false;
}
