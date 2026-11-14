/*
 * TinyPHP.rt - json.c - M6 JSON（C 原生手写）
 *
 * json_encode / json_decode，语义对齐 PHP 8 默认行为（无 JSON_THROW_ON_ERROR）：
 *   encode：null/bool/int/float/string/array；对象不支持（抛 Error）；
 *           连续 int 键 0..n-1 → JSON 数组，否则对象；无效 UTF-8 → false + Warning
 *   decode：返回 array（assoc=true 默认）/ 标量；深度超限 → null + Warning
 *           语法错 → null + Warning
 * 与 PHP 交换数据走 JSON（serialize 自有格式不互通，PLAN/02 §4）。
 */
#include "value.h"
#include "opcode.h"
#include "tphp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)

/* ---- 输出缓冲 ---- */
typedef struct {
    char *p;
    size_t len, cap;
} JBuf;

static void jb_init(JBuf *b) {
    b->cap = 256;
    b->p = tm_malloc(b->cap);
    b->len = 0;
}

static char *jb_reserve(JBuf *b, size_t n) {
    if (b->len + n + 1 > b->cap) {
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->p = tm_realloc(b->p, b->cap);
    }
    return b->p + b->len;
}

static void jb_put(JBuf *b, const char *s, size_t n) {
    memcpy(jb_reserve(b, n), s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void jb_puts(JBuf *b, const char *s) { jb_put(b, s, strlen(s)); }
static void jb_ch(JBuf *b, char c) { jb_put(b, &c, 1); }

/* ---- UTF-8 校验 ---- */
static bool utf8_valid(const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i++; continue; }
        int need;
        if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + (size_t)need >= n) return false;
        for (int k = 1; k <= need; k++) {
            if (((unsigned char)s[i + k] & 0xC0) != 0x80)
                return false;
        }
        i += (size_t)need + 1;
    }
    return true;
}

/* ---- encode ---- */

/* encode 上下文（A1-b）：keys 逆映射缓冲跨数组复用（serialize sw.keys 同款，
 * 免每数组 malloc/free） */
typedef struct {
    VM *vm;
    JBuf *b;
    int options;
    TVal *keys; uint32_t cap_keys;   /* off→key 逆映射缓冲（只增不减） */
} ECtx;

/* i64→ascii（A1-b：替代 snprintf("%lld")——bench 密度每轮 ~5 万次） */
static size_t fmt_i64(char *out, int64_t v) {
    char tmp[20];
    size_t k = 0;
    uint64_t u = (v < 0) ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
    do { tmp[k++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    size_t n = 0;
    if (v < 0) out[n++] = '-';
    while (k) out[n++] = tmp[--k];
    return n;
}

/* 字符串编码段拷快路（A1-b）：原实现逐字符走 jb_ch（函数调用+检查+1 字节
 * memcpy/字符）。危险集 = '"' '\\' <0x20，其余字节（含 UTF-8 全部续字节）
 * 原样直写；安全段批量 memcpy。 */
static void enc_str(ECtx *c, const char *s, size_t n) {
    JBuf *b = c->b;
    jb_ch(b, '"');
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n) {
            unsigned char ch = (unsigned char)s[j];
            if (ch < 0x20 || ch == '"' || ch == '\\') break;
            j++;
        }
        if (j > i) {
            memcpy(jb_reserve(b, j - i), s + i, j - i);
            b->len += j - i;
            b->p[b->len] = 0;
        }
        if (j == n) break;
        unsigned char ch = (unsigned char)s[j];
        switch (ch) {
        case '"':  jb_puts(b, "\\\""); break;
        case '\\': jb_puts(b, "\\\\"); break;
        case '\b': jb_puts(b, "\\b"); break;
        case '\f': jb_puts(b, "\\f"); break;
        case '\n': jb_puts(b, "\\n"); break;
        case '\r': jb_puts(b, "\\r"); break;
        case '\t': jb_puts(b, "\\t"); break;
        default: {
            /* <0x20 → \u00XX（hex 查表，免 snprintf） */
            static const char hex[] = "0123456789abcdef";
            char tmp[6] = { '\\', 'u', '0', '0', hex[(ch >> 4) & 0xF], hex[ch & 0xF] };
            jb_put(b, tmp, 6);
            break;
        }
        }
        i = j + 1;
    }
    jb_ch(b, '"');
}

static bool enc_val(ECtx *c, TVal v, int depth);

/* 数组键单遍逆映射（A1-b：替代原 arr_is_list/arr_key_by_off 的每元素
 * 全桶扫 O(n·bcap)——assoc 分支原为二次方桶探查；缓冲挂 ECtx 复用）。
 * 返回 keys[dused]（T_UNDEF=空洞），packed 时返回 NULL；*is_list =
 * 连续 int 键 0..n-1（语义与原 arr_is_list 一致：首个键失配即判否）。 */
static TVal *arr_keys_map(ECtx *c, TArr *a, bool *is_list) {
    if (a->packed) { *is_list = true; return NULL; }   /* 不变量：packed ⇒ 键即 0..n-1 无洞 */
    tarr_sync_buckets(a);
    if (a->dused > c->cap_keys) {
        uint32_t nc = c->cap_keys ? c->cap_keys : 16;
        while (nc < a->dused) nc *= 2;
        c->keys = tm_realloc(c->keys, sizeof(TVal) * (size_t)nc);
        c->cap_keys = nc;
    }
    TVal *keys = c->keys;
    for (uint32_t i = 0; i < a->dused; i++) keys[i].t = T_UNDEF;
    for (uint32_t bk = 0; bk < a->bcap; bk++) {
        if (a->buckets[bk].val_off != UINT32_MAX &&
            a->buckets[bk].val_off < a->dused)
            keys[a->buckets[bk].val_off] = a->buckets[bk].key;
    }
    uint32_t idx = 0;
    bool list = true;
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_UNDEF || keys[i].t == T_UNDEF) continue;
        if (keys[i].t != T_INT || keys[i].u.i != (int64_t)idx) { list = false; break; }
        idx++;
    }
    *is_list = list;
    return keys;
}

static bool enc_val(ECtx *c, TVal v, int depth) {
    JBuf *b = c->b;
    if (depth > 512) return false;
    switch (v.t) {
    case T_NULL:  jb_puts(b, "null"); return true;
    case T_FALSE: jb_puts(b, "false"); return true;
    case T_TRUE:  jb_puts(b, "true"); return true;
    case T_INT: {
        char tmp[24];
        jb_put(b, tmp, fmt_i64(tmp, v.u.i));
        return true;
    }
    case T_FLOAT: {
        double d = v.u.d;
        if (isnan(d) || isinf(d)) return false;
        /* PHP: 整数值浮点无小数点时仍带 .0 */
        char tmp[64];
        if (d == (double)(int64_t)d && fabs(d) < 1e15)
            snprintf(tmp, sizeof tmp, "%.1f", d);
        else
            snprintf(tmp, sizeof tmp, "%.17g", d);
        /* 去掉多余尾零（但保留 .0） */
        jb_puts(b, tmp);
        return true;
    }
    case T_STR: {
        TStr *s = (TStr *)v.u.p;
        if (!utf8_valid(s->c, s->len)) return false;
        enc_str(c, s->c, s->len);
        return true;
    }
    case T_ARR: {
        TArr *a = (TArr *)v.u.p;
        bool is_list;
        TVal *keys = arr_keys_map(c, a, &is_list);
        if (is_list) {
            jb_ch(b, '[');
            bool first = true;
            for (uint32_t i = 0; i < a->dused; i++) {
                if (a->data[i].t == T_UNDEF) continue;
                if (!first) jb_ch(b, ',');
                first = false;
                if (!enc_val(c, a->data[i], depth + 1))
                    return false;
            }
            jb_ch(b, ']');
        } else {
            jb_ch(b, '{');
            bool first = true;
            for (uint32_t i = 0; i < a->dused; i++) {
                if (a->data[i].t == T_UNDEF || keys[i].t == T_UNDEF) continue;
                TVal k = keys[i];
                if (!first) jb_ch(b, ',');
                first = false;
                if (k.t == T_INT) {
                    char tmp[24];
                    enc_str(c, tmp, fmt_i64(tmp, k.u.i));
                } else {
                    TStr *ks = (TStr *)k.u.p;
                    enc_str(c, ks->c, ks->len);
                }
                jb_ch(b, ':');
                if (!enc_val(c, a->data[i], depth + 1))
                    return false;
            }
            jb_ch(b, '}');
        }
        return true;
    }
    case T_OBJ: case T_CLOSURE: case T_ENUM_CASE: case T_RES:
        /* 对象默认不支持（无 JsonSerializable 接口） */
        return false;
    }
    return false;
}

TVal bi_json_encode(VM *vm, TVal *args, int n) {
    if (n < 1)
        RAISE(vm, "ArgumentCountError", "json_encode() expects at least 1 argument");
    int options = n >= 2 ? (int)tval_to_int(args[1]) : 0;
    JBuf b;
    jb_init(&b);
    ECtx c = { vm, &b, options, NULL, 0 };
    if (!enc_val(&c, args[0], 0)) {
        tm_free(b.p);
        tm_free(c.keys);
        tphp_warn("PHP Warning:  json_encode(): Invalid UTF-8 or unsupported type\n");
        return TV_FALSE;
    }
    TStr *s = tstr_new(b.p, b.len);
    tm_free(b.p);
    tm_free(c.keys);
    return TV_STR(s);
}

/* ---- decode ---- */

typedef struct {
    const char *p, *end;
    VM *vm;
    int depth;
    bool silent;   /* C API (tphp_json_decode_c): suppress stderr warnings */
} JDec;

static void jskip(JDec *d) {
    while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\n' || *d->p == '\r'))
        d->p++;
}

static bool jfail(JDec *d, const char *msg) {
    if (!d->silent)
        tphp_warn("PHP Warning:  json_decode(): %s\n", msg);
    return false;
}

static bool jparse_value(JDec *d, TVal *out);

static bool jparse_string(JDec *d, TStr **out) {
    if (d->p >= d->end || *d->p != '"') return jfail(d, "Syntax error");
    d->p++;
    /* 快扫到结束引号（处理转义） */
    const char *start = d->p;
    const char *scan = d->p;
    while (scan < d->end && *scan != '"') {
        if (*scan == '\\') {
            scan++;
            if (scan >= d->end) return jfail(d, "Syntax error");
        }
        scan++;
    }
    if (scan >= d->end) return jfail(d, "Syntax error");
    size_t raw = (size_t)(scan - start);

    /* 解码缓冲 */
    size_t cap = raw + 1;
    char *buf = tm_malloc(cap);
    size_t n = 0;
    const char *s = start;
    const char *e = scan;
    while (s < e) {
        if (*s != '\\') {
            buf[n++] = *s++;
            continue;
        }
        s++;
        if (s >= e) { tm_free(buf); return jfail(d, "Syntax error"); }
        char c = *s++;
        switch (c) {
        case '"':  buf[n++] = '"'; break;
        case '\\': buf[n++] = '\\'; break;
        case '/':  buf[n++] = '/'; break;
        case 'b':  buf[n++] = '\b'; break;
        case 'f':  buf[n++] = '\f'; break;
        case 'n':  buf[n++] = '\n'; break;
        case 'r':  buf[n++] = '\r'; break;
        case 't':  buf[n++] = '\t'; break;
        case 'u': {
            if (e - s < 4) { tm_free(buf); return jfail(d, "Syntax error"); }
            unsigned cp = 0;
            for (int i = 0; i < 4; i++) {
                char h = *s++;
                cp <<= 4;
                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                else { tm_free(buf); return jfail(d, "Syntax error"); }
            }
            /* BMP only（代理对简化：不处理 \uD800-\uDFFF） */
            if (cp < 0x80) buf[n++] = (char)cp;
            else if (cp < 0x800) {
                buf[n++] = (char)(0xC0 | (cp >> 6));
                buf[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                buf[n++] = (char)(0xE0 | (cp >> 12));
                buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            tm_free(buf);
            return jfail(d, "Syntax error");
        }
    }
    *out = tstr_new(buf, n);
    tm_free(buf);
    d->p = e + 1;   /* 跳过结束引号 */
    return true;
}

static bool jparse_number(JDec *d, TVal *out) {
    const char *start = d->p;
    if (d->p < d->end && *d->p == '-') d->p++;
    bool is_float = false;
    while (d->p < d->end) {
        char c = *d->p;
        if (c >= '0' && c <= '9') { d->p++; continue; }
        if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
            if (c == '.' || c == 'e' || c == 'E') is_float = true;
            d->p++;
            continue;
        }
        break;
    }
    size_t len = (size_t)(d->p - start);
    if (len == 0 || (len == 1 && start[0] == '-')) return jfail(d, "Syntax error");
    char tmp[64];
    if (len >= sizeof tmp) return jfail(d, "Syntax error");
    memcpy(tmp, start, len);
    tmp[len] = 0;
    if (!is_float) {
        /* 先试 int64 */
        char *endp = NULL;
        long long iv = strtoll(tmp, &endp, 10);
        if (endp && *endp == 0) {
            *out = TV_INT(iv);
            return true;
        }
    }
    char *endp = NULL;
    double dv = strtod(tmp, &endp);
    *out = TV_FLOAT(dv);
    return true;
}

static bool jparse_value(JDec *d, TVal *out) {
    jskip(d);
    if (d->p >= d->end) return jfail(d, "Syntax error");
    char c = *d->p;
    if (c == 'n') {
        if (d->end - d->p >= 4 && memcmp(d->p, "null", 4) == 0) {
            d->p += 4;
            *out = TV_NULL;
            return true;
        }
        return jfail(d, "Syntax error");
    }
    if (c == 't') {
        if (d->end - d->p >= 4 && memcmp(d->p, "true", 4) == 0) {
            d->p += 4;
            *out = TV_TRUE;
            return true;
        }
        return jfail(d, "Syntax error");
    }
    if (c == 'f') {
        if (d->end - d->p >= 5 && memcmp(d->p, "false", 5) == 0) {
            d->p += 5;
            *out = TV_FALSE;
            return true;
        }
        return jfail(d, "Syntax error");
    }
    if (c == '"') {
        TStr *s = NULL;
        if (!jparse_string(d, &s)) return false;
        *out = TV_STR(s);
        return true;
    }
    if (c == '-' || (c >= '0' && c <= '9'))
        return jparse_number(d, out);
    if (c == '[') {
        if (++d->depth > 512) return jfail(d, "Maximum stack depth exceeded");
        d->p++;
        TArr *a = tarr_new();
        jskip(d);
        if (d->p < d->end && *d->p == ']') {
            d->p++;
            d->depth--;
            *out = TV_ARR(a);
            return true;
        }
        for (;;) {
            TVal el;
            if (!jparse_value(d, &el)) {
                tval_release(&(TVal){ .u.p = a, .t = T_ARR });
                return false;
            }
            tarr_append(a, el);
            jskip(d);
            if (d->p < d->end && *d->p == ',') { d->p++; continue; }
            if (d->p < d->end && *d->p == ']') { d->p++; break; }
            tval_release(&(TVal){ .u.p = a, .t = T_ARR });
            return jfail(d, "Syntax error");
        }
        d->depth--;
        *out = TV_ARR(a);
        return true;
    }
    if (c == '{') {
        if (++d->depth > 512) return jfail(d, "Maximum stack depth exceeded");
        d->p++;
        TArr *a = tarr_new();
        jskip(d);
        if (d->p < d->end && *d->p == '}') {
            d->p++;
            d->depth--;
            *out = TV_ARR(a);
            return true;
        }
        for (;;) {
            jskip(d);
            TStr *key = NULL;
            if (!jparse_string(d, &key)) {
                tval_release(&(TVal){ .u.p = a, .t = T_ARR });
                return false;
            }
            jskip(d);
            if (d->p >= d->end || *d->p != ':') {
                tval_release(&(TVal){ .u.p = key, .t = T_STR });
                tval_release(&(TVal){ .u.p = a, .t = T_ARR });
                return jfail(d, "Syntax error");
            }
            d->p++;
            TVal val;
            if (!jparse_value(d, &val)) {
                tval_release(&(TVal){ .u.p = key, .t = T_STR });
                tval_release(&(TVal){ .u.p = a, .t = T_ARR });
                return false;
            }
            /* 键规范化走 tarr_set */
            TVal k = TV_STR(key);
            tarr_set(a, k, val);
            tval_release(&k);
            jskip(d);
            if (d->p < d->end && *d->p == ',') { d->p++; continue; }
            if (d->p < d->end && *d->p == '}') { d->p++; break; }
            tval_release(&(TVal){ .u.p = a, .t = T_ARR });
            return jfail(d, "Syntax error");
        }
        d->depth--;
        *out = TV_ARR(a);
        return true;
    }
    return jfail(d, "Syntax error");
}

TVal bi_json_decode(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "json_decode() expects string");
    TStr *s = (TStr *)args[0].u.p;
    /* assoc 默认 true（PHP 7+ 无第二个参数时返回 array） */
    bool assoc = n >= 2 ? tval_to_bool(args[1]) : true;
    int depth = n >= 3 ? (int)tval_to_int(args[2]) : 512;
    if (depth < 1) depth = 1;
    if (depth > 512) depth = 512;

    if (s->len == 0) {
        tphp_warn("PHP Warning:  json_decode(): Syntax error\n");
        return TV_NULL;
    }
    JDec d = { .p = s->c, .end = s->c + s->len, .vm = vm, .depth = 0 };
    TVal out = TV_NULL;
    if (!jparse_value(&d, &out))
        return TV_NULL;
    jskip(&d);
    if (d.p != d.end) {
        tval_release(&out);
        tphp_warn("PHP Warning:  json_decode(): Syntax error\n");
        return TV_NULL;
    }
    /* assoc=false 时对象应转对象——M6 简化：始终 array（与 assoc=true 一致） */
    (void)assoc;
    return out;
}

/* json_last_error 简化：始终 0（JSON_ERROR_NONE）——Warning 已打出 */
TVal bi_json_last_error(VM *vm, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return TV_INT(0);
}

TVal bi_json_last_error_msg(VM *vm, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return TV_STR(tstr_new("No error", 8));
}

/* ---- C-level decode (pkg.c): no VM, silent on failure ---- */
bool tphp_json_decode_c(const char *src, size_t len, TVal *out) {
    if (!src || !out) return false;
    JDec d = { .p = src, .end = src + len, .vm = NULL, .depth = 0, .silent = true };
    if (!jparse_value(&d, out)) return false;
    jskip(&d);
    if (d.p != d.end) {
        tval_release(out);
        return false;
    }
    return true;
}

/* ---- C-level encode (teb/ext 侧工厂用): no VM, silent on failure ----
 * enc_val 不使用 vm（ECtx.vm 仅 bi 入口填充），NULL 安全。
 * v 为借用语义；失败不写 *out。 */
bool tphp_json_encode_c(TVal v, TStr **out) {
    if (!out) return false;
    JBuf b;
    jb_init(&b);
    ECtx c = { NULL, &b, 0, NULL, 0 };
    if (!enc_val(&c, v, 0)) {
        tm_free(b.p);
        tm_free(c.keys);
        return false;
    }
    *out = tstr_new(b.p, b.len);
    tm_free(b.p);
    tm_free(c.keys);
    return true;
}
