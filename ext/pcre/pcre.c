/*
 * TinyPHP.rt - pcre.c - preg_* (PLAN/preg.md; php_pcre.c 语义基准)
 *
 * 错误模型：编译失败/坏分隔符/未知修饰符 = 异常（tphp 诊断即异常）；
 * 运行时 backtrack/recursion limit = PHP 返回值协议（false/null/部分结果）
 * + preg_last_error（PHP 该路径本无 Warning）。
 * $matches 无引用适配：要求已初始化数组，原地清空填充（sort 同哲学）。
 * v1 不启用 PCRE2 JIT。
 */
#include "tphp.h"
#include "value.h"
#include "class.h"
#include "pcre.h"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <string.h>
#include <stdio.h>

#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)

/* ---- error_code（TLS，PHP PCRE_G(error_code) 对应） ---- */
enum {
    PREG_NO_ERROR = 0,
    PREG_INTERNAL_ERROR = 1,
    PREG_BACKTRACK_LIMIT_ERROR = 2,
    PREG_RECURSION_LIMIT_ERROR = 3,
    PREG_JIT_STACKLIMIT_ERROR = 4,
    PREG_LIMIT_UTF8_ERROR = 5,
};
static _Thread_local int g_preg_err;

/* ---- 编译缓存（TLS 链表；CLI 短进程无上限简单版） ---- */
typedef struct PCreEnt {
    char *key;            /* "pattern\x1fmodifiers" */
    pcre2_code *code;
    struct PCreEnt *next;
} PCreEnt;
static _Thread_local PCreEnt *g_cache;
static _Thread_local int g_cache_n;

static char *xstrdup_local(const char *s) {
    size_t n = strlen(s);
    char *r = tm_malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

/* 简易字节缓冲（本文件私有） */
typedef struct { char *p; size_t len, cap; } PBuf;
static void pb_init(PBuf *b) { b->p = NULL; b->len = b->cap = 0; }
static void pb_free(PBuf *b) { tm_free(b->p); b->p = NULL; b->len = b->cap = 0; }
static void pb_put(PBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 128;
        while (nc < b->len + n + 1) nc *= 2;
        b->p = tm_realloc(b->p, nc);
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void cache_put(const char *key, pcre2_code *code) {
    PCreEnt *e = tm_malloc(sizeof *e);
    e->key = xstrdup_local(key);
    e->code = code;
    e->next = g_cache;
    g_cache = e;
    g_cache_n++;
}

static PCreEnt *cache_find(const char *key) {
    for (PCreEnt *e = g_cache; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

/* ---- 分隔符 + 修饰符解析（php_pcre.c L620-749 语义） ---- */

/* 成对分隔符：返回结束分隔符字符；单字符返回自身 */
static char pair_close(char open) {
    switch (open) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '<': return '>';
    default: return 0;
    }
}

/* 从 pattern 提取 body（不含分隔符）与修饰符 → PCRE2 选项。
 * 成功 body/opts 输出；失败 RAISE（编译类=异常）。 */
static bool parse_pattern(VM *vm, const char *pat, size_t patlen,
                          char **out_body, uint32_t *out_copts,
                          const char *fname) {
    size_t i = 0;
    while (i < patlen && (pat[i] == ' ' || pat[i] == '\t' ||
                          pat[i] == '\n' || pat[i] == '\r'))
        i++;
    if (i >= patlen)
        RAISE(vm, "ValueError", "%s(): empty pattern", fname);
    char delim = pat[i];
    if ((delim >= 'A' && delim <= 'Z') || (delim >= 'a' && delim <= 'z') ||
        (delim >= '0' && delim <= '9') || delim == '\\' || delim == 0)
        RAISE(vm, "ValueError",
              "%s(): Delimiter must not be alphanumeric, backslash, or NUL", fname);
    i++;
    char close = pair_close(delim);
    size_t body_start = i;
    size_t brackets = 0;
    size_t body_end = patlen;
    if (close) {
        /* 成对分隔符：内层同类 open 计数；遇 close 且 brackets==0 结束 */
        while (i < patlen) {
            if (pat[i] == '\\') { i += 2; continue; }
            if (pat[i] == delim) brackets++;
            else if (pat[i] == close) {
                if (brackets == 0) { body_end = i; break; }
                brackets--;
            }
            i++;
        }
    } else {
        while (i < patlen) {
            if (pat[i] == '\\') { i += 2; continue; }
            if (pat[i] == delim) { body_end = i; break; }
            i++;
        }
    }
    if (body_end >= patlen || pat[body_end] != delim)
        RAISE(vm, "ValueError", "%s(): No ending delimiter '%c' found", fname, delim);
    if (close && brackets > 0)
        RAISE(vm, "ValueError", "%s(): Unmatched ( in pattern", fname);

    uint32_t copts = 0;   /* php_pcre.c L582 同款从 0 起步 */
    size_t j = body_end + 1;
    while (j < patlen) {
        char m = pat[j];
        switch (m) {
        case 'i': copts |= PCRE2_CASELESS; break;
        case 'm': copts |= PCRE2_MULTILINE; break;
        case 'n': copts |= PCRE2_NO_AUTO_CAPTURE; break;
        case 's': copts |= PCRE2_DOTALL; break;
        case 'x': copts |= PCRE2_EXTENDED; break;
        case 'A': copts |= PCRE2_ANCHORED; break;
        case 'D': copts |= PCRE2_DOLLAR_ENDONLY; break;
        case 'U': copts |= PCRE2_UNGREEDY; break;
        case 'u': copts |= PCRE2_UTF | PCRE2_UCP; break;
        case 'J': copts |= PCRE2_DUPNAMES; break;
        case 'S': case 'X': case ' ': case '\t': break;   /* no-op */
        default:
            RAISE(vm, "ValueError", "%s(): Unknown modifier '%c'", fname, m);
        }
        j++;
    }

    size_t blen = body_end - body_start;
    char *body = tm_malloc(blen + 1);
    memcpy(body, pat + body_start, blen);
    body[blen] = 0;
    *out_body = body;
    *out_copts = copts;
    return true;
}

/* 编译（经缓存）。失败已 RAISE。 */
static pcre2_code *compile_pattern(VM *vm, const char *pat, size_t patlen,
                                   const char *fname) {
    char *body = NULL;
    uint32_t copts = 0;
    parse_pattern(vm, pat, patlen, &body, &copts, fname);

    /* cache key = 原 pattern 全串 */
    char key[1032];
    snprintf(key, sizeof key, "%.*s", (int)(patlen < 1000 ? patlen : 1000), pat);
    PCreEnt *hit = cache_find(key);
    if (hit) { tm_free(body); return hit->code; }

    int errcode = 0;
    PCRE2_SIZE erroff = 0;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)body, PCRE2_ZERO_TERMINATED,
                                     copts, &errcode, &erroff, NULL);
    tm_free(body);
    if (!code) {
        g_preg_err = PREG_INTERNAL_ERROR;
        PCRE2_UCHAR errbuf[256];
        pcre2_get_error_message(errcode, errbuf, sizeof errbuf);
        RAISE(vm, "Error", "%s(): Compilation failed: %s at offset %zu",
              fname, (char *)errbuf, (size_t)erroff);
    }
    cache_put(key, code);
    return code;
}

/* ---- matches 数组填充（无引用适配：原地清空共享数组） ---- */

static TArr *matches_arr(VM *vm, TVal *slot, const char *fname) {
    if (slot->t != T_ARR)
        RAISE(vm, "TypeError",
              "%s(): $matches must be an already-initialized array "
              "(tphp has no references; write $m = []; first)", fname);
    TArr *a = (TArr *)slot->u.p;
    /* 清空（复用 bi_sort 的原地清空） */
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t != T_UNDEF) tval_release(&a->data[i]);
        a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    for (uint32_t b = 0; b < a->bcap; b++) {
        if (a->buckets[b].val_off != UINT32_MAX && a->buckets[b].key.t == T_STR)
            tval_release(&a->buckets[b].key);
        a->buckets[b].key = TV_NULL;
        a->buckets[b].val_off = UINT32_MAX;
    }
    a->n = 0;
    a->next_idx = 0;
    a->free_head = UINT32_MAX;
    a->dused = 0;
    return a;
}

/* flags（PHP 值） */
#define PHP_PREG_OFFSET_CAPTURE    256
#define PHP_PREG_UNMATCHED_AS_NULL 512
#define PHP_PREG_SET_ORDER         1

/* 单次匹配结果 → ma（数字组 + 命名组键）。
 * UNMATCHED_AS_NULL：补齐全部声明组（未参与=null）；否则截断到 rc。
 * OFFSET_CAPTURE：每项 [value, byte_offset]。 */
static void fill_matches(TArr *ma, pcre2_code *code, pcre2_match_data *md,
                         const char *subj, int rc, int64_t flags) {
    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
    uint32_t ovec_count = pcre2_get_ovector_count(md);
    bool unmatched_null = (flags & PHP_PREG_UNMATCHED_AS_NULL) != 0;
    bool offset_cap = (flags & PHP_PREG_OFFSET_CAPTURE) != 0;
    uint32_t cap_count = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &cap_count);

    for (uint32_t gi = 0; gi <= cap_count; gi++) {
        if (!unmatched_null && gi >= (uint32_t)rc) break;   /* 尾组截断（PHP） */
        uint32_t g = gi * 2;
        if (g + 1 >= ovec_count * 2) break;
        bool unmatched = (ov[g] == PCRE2_UNSET);
        TVal item;
        if (unmatched) {
            item = unmatched_null ? TV_NULL : TV_STR(tstr_new("", 0));
        } else {
            item = TV_STR(tstr_new(subj + ov[g], (size_t)(ov[g + 1] - ov[g])));
        }
        if (offset_cap) {
            TArr *pair = tarr_new();
            tarr_append(pair, item);
            tarr_append(pair, unmatched ? TV_INT(-1) : TV_INT((int64_t)ov[g]));
            item = TV_ARR(pair);
        }
        tarr_append(ma, item);
    }

    /* 命名组：数字组之外加同名键（DUPNAMES 时跳过已有非空值的键——优先
     * 有值者，PHP 语义近似） */
    PCRE2_SPTR name_table = NULL;
    uint32_t name_entry_size = 0, name_count = 0;
    pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &name_table);
    pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &name_entry_size);
    pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &name_count);
    for (uint32_t i = 0; i < name_count; i++) {
        const unsigned char *entry = (const unsigned char *)name_table +
                                     i * name_entry_size;
        uint32_t gnum = ((uint32_t)entry[0] << 8) | entry[1];
        const char *nm = (const char *)(entry + 2);
        TVal k = TV_STR(tstr_new(nm, strlen(nm)));
        if (tarr_has(ma, k)) {
            TVal *ex = tarr_get(ma, k);
            bool empty = ex && ((ex->t == T_STR && tstr_len((TStr *)ex->u.p) == 0)
                                || ex->t == T_NULL);
            if (!empty) { tval_release(&k); continue; }   /* 已有非空：保留 */
            if (ex) { tval_release(ex); tarr_unset(ma, k); }
        }
        uint32_t g = (uint32_t)gnum * 2;
        if (g + 1 >= ovec_count * 2) { tval_release(&k); continue; }
        bool unmatched = (ov[g] == PCRE2_UNSET);
        TVal item;
        if (unmatched) item = unmatched_null ? TV_NULL : TV_STR(tstr_new("", 0));
        else item = TV_STR(tstr_new(subj + ov[g], (size_t)(ov[g + 1] - ov[g])));
        if (offset_cap) {
            TArr *pair = tarr_new();
            tarr_append(pair, item);
            tarr_append(pair, unmatched ? TV_INT(-1) : TV_INT((int64_t)ov[g]));
            item = TV_ARR(pair);
        }
        tarr_set(ma, k, item);
        tval_release(&k);
    }
}

/* 运行错误码 → g_preg_err；返回 false/部分策略由调用方定 */
static bool map_match_rc(int rc) {
    if (rc >= 0) { g_preg_err = PREG_NO_ERROR; return true; }
    if (rc == PCRE2_ERROR_NOMATCH) { g_preg_err = PREG_NO_ERROR; return false; }
    g_preg_err = (rc == PCRE2_ERROR_MATCHLIMIT) ? PREG_BACKTRACK_LIMIT_ERROR
               : (rc == PCRE2_ERROR_DEPTHLIMIT) ? PREG_RECURSION_LIMIT_ERROR
               : PREG_INTERNAL_ERROR;
    return false;
}

/* ---- preg_match ---- */

TVal bi_preg_match(VM *vm, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR || args[1].t != T_STR)
        RAISE(vm, "TypeError", "preg_match() expects (string $pattern, string $subject, ...)");
    const char *pat = tstr_val((TStr *)args[0].u.p);
    size_t patlen = tstr_len((TStr *)args[0].u.p);
    const char *subj = tstr_val((TStr *)args[1].u.p);
    size_t sublen = tstr_len((TStr *)args[1].u.p);
    TArr *ma = NULL;
    if (n >= 3 && args[2].t != T_NULL)
        ma = matches_arr(vm, &args[2], "preg_match");
    int64_t flags = n >= 4 ? tval_to_int(args[3]) : 0;
    int64_t offset = n >= 5 ? tval_to_int(args[4]) : 0;

    if (offset < 0) offset += (int64_t)sublen;
    if (offset < 0 || (size_t)offset > sublen) {
        g_preg_err = PREG_LIMIT_UTF8_ERROR;
        return TV_FALSE;
    }

    pcre2_code *code = compile_pattern(vm, pat, patlen, "preg_match");
    pcre2_match_context *mctx = pcre2_match_context_create(NULL);
    pcre2_set_match_limit(mctx, 1000000);
    pcre2_set_depth_limit(mctx, 100000);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    int rc = pcre2_match(code, (PCRE2_SPTR)subj, sublen,
                         (PCRE2_SIZE)offset, 0, md, mctx);
    bool ok = map_match_rc(rc);
    if (rc >= 0 && ma)
        fill_matches(ma, code, md, subj, rc, flags);
    pcre2_match_data_free(md);
    pcre2_match_context_free(mctx);
    if (rc == PCRE2_ERROR_NOMATCH) return TV_INT(0);
    return ok ? TV_INT(1) : TV_FALSE;
}

/* ---- preg_match_all ---- */

TVal bi_preg_match_all(VM *vm, TVal *args, int n) {
    if (n < 3 || args[0].t != T_STR || args[1].t != T_STR)
        RAISE(vm, "TypeError",
              "preg_match_all() expects (string $pattern, string $subject, array &$matches, ...)");
    const char *pat = tstr_val((TStr *)args[0].u.p);
    size_t patlen = tstr_len((TStr *)args[0].u.p);
    const char *subj = tstr_val((TStr *)args[1].u.p);
    size_t sublen = tstr_len((TStr *)args[1].u.p);
    TArr *ma = matches_arr(vm, &args[2], "preg_match_all");
    int64_t flags = n >= 4 ? tval_to_int(args[3]) : 0;
    int64_t offset = n >= 5 ? tval_to_int(args[4]) : 0;
    bool set_order = (flags & PHP_PREG_SET_ORDER) != 0;

    if (offset < 0) offset += (int64_t)sublen;
    if (offset < 0 || (size_t)offset > sublen) {
        g_preg_err = PREG_LIMIT_UTF8_ERROR;
        return TV_FALSE;
    }

    pcre2_code *code = compile_pattern(vm, pat, patlen, "preg_match_all");
    pcre2_match_context *mctx = pcre2_match_context_create(NULL);
    pcre2_set_match_limit(mctx, 1000000);
    pcre2_set_depth_limit(mctx, 100000);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    g_preg_err = PREG_NO_ERROR;

    int count = 0;
    PCRE2_SIZE cur = (PCRE2_SIZE)offset;
    while (cur <= sublen) {
        int rc = pcre2_match(code, (PCRE2_SPTR)subj, sublen, cur, 0, md, mctx);
        if (rc == PCRE2_ERROR_NOMATCH) break;
        if (rc < 0) { map_match_rc(rc); break; }
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        PCRE2_SIZE s = ov[0], e = ov[1];
        /* 单次结果装入临时数组（含命名组） */
        TArr *one = tarr_new();
        fill_matches(one, code, md, subj, rc, flags);
        if (set_order) {
            tarr_append(ma, TV_ARR(one));      /* 所有权转移 */
        } else {
            /* PATTERN_ORDER：gi 聚合到 ma[gi][]（PHP：每个组一个扁平数组） */
            uint32_t cnt = tarr_count(one);
            for (uint32_t gi = 0; gi < cnt; gi++) {
                TVal gk = TV_INT((int64_t)gi);
                TVal *item = tarr_get(one, gk);
                TVal sub = item ? *item : TV_NULL;
                if (item) tval_addref(sub);
                TVal *bp = tarr_get(ma, gk);
                TArr *bucket;
                if (!bp) {
                    bucket = tarr_new();
                    tarr_set(ma, gk, TV_ARR(bucket));   /* 接管所有权，勿再 release */
                    bp = tarr_get(ma, gk);
                }
                bucket = (TArr *)bp->u.p;
                tarr_append(bucket, sub);      /* 所有权转移 */
            }
            /* 释放 one 容器及其元素（上面已各自 addref） */
            tval_release(&(TVal){ .u.p = one, .t = T_ARR });
        }
        count++;
        /* 空匹配推进（PHP /g 近似：NOTEMPTY_ATSTART|ANCHORED 重试） */
        if (s == e) {
            if (s == sublen) break;
            cur = s + 1;
        } else {
            cur = e;
        }
    }
    pcre2_match_data_free(md);
    pcre2_match_context_free(mctx);
    return TV_INT(count);
}

/* ---- preg_last_error / _msg ---- */

TVal bi_preg_last_error(VM *vm, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return TV_INT(g_preg_err);
}

TVal bi_preg_last_error_msg(VM *vm, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    const char *m;
    switch (g_preg_err) {
    case PREG_NO_ERROR: m = "No error"; break;
    case PREG_INTERNAL_ERROR: m = "Internal error"; break;
    case PREG_BACKTRACK_LIMIT_ERROR: m = "Backtrack limit exhausted"; break;
    case PREG_RECURSION_LIMIT_ERROR: m = "Recursion limit exhausted"; break;
    case PREG_JIT_STACKLIMIT_ERROR: m = "JIT stack limit exhausted"; break;
    case PREG_LIMIT_UTF8_ERROR: m = "Malformed UTF-8 characters in offset or subject"; break;
    default: m = "Unknown error"; break;
    }
    return TV_STR(tstr_new(m, strlen(m)));
}

/* preg_quote：正则元字符前加 \（PHP 字符集 .\+*?[^]$(){}=!<>|:-# 与分隔符） */
TVal bi_preg_quote(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_STR)
        RAISE(vm, "TypeError", "preg_quote() expects string $str");
    const char *s = tstr_val((TStr *)args[0].u.p);
    size_t len = tstr_len((TStr *)args[0].u.p);
    char delim = (n >= 2 && args[1].t == T_STR && tstr_len((TStr *)args[1].u.p))
                 ? tstr_val((TStr *)args[1].u.p)[0] : 0;
    PBuf out;
    pb_init(&out);
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool meta = c == '.' || c == '\\' || c == '+' || c == '*' || c == '?' ||
                    c == '[' || c == ']' || c == '^' || c == '$' || c == '(' ||
                    c == ')' || c == '{' || c == '}' || c == '=' || c == '!' ||
                    c == '<' || c == '>' || c == '|' || c == ':' || c == '-' ||
                    c == '#' || (delim && c == delim);
        if (meta) pb_put(&out, "\\", 1);
        pb_put(&out, &c, 1);
    }
    TVal r = TV_STR(tstr_new(out.p ? out.p : "", out.len));
    pb_free(&out);
    return r;
}

/* ---- P3: preg_replace / preg_split / preg_grep ---- */

#define PHP_PREG_SPLIT_NO_EMPTY      1
#define PHP_PREG_SPLIT_DELIM_CAPTURE 2
#define PHP_PREG_SPLIT_OFFSET_CAPTURE 4
#define PHP_PREG_GREP_INVERT 1

/* ---- P3: preg_replace / preg_split / preg_grep ---- */
static void expand_replacement(PBuf *out, const char *rep, size_t replen,
                               PCRE2_SIZE *ov, uint32_t ovec_count,
                               const char *subj) {
    size_t i = 0;
    while (i < replen) {
        if (rep[i] == '$' && i + 1 < replen) {
            int gnum = -1;
            size_t consumed = 0;
            if (rep[i + 1] == '{') {
                size_t j = i + 2;
                int g = 0, nd = 0;
                while (j < replen && rep[j] >= '0' && rep[j] <= '9' && nd < 2) {
                    g = g * 10 + (rep[j] - '0');
                    j++; nd++;
                }
                if (nd > 0 && j < replen && rep[j] == '}') {
                    gnum = g;
                    consumed = j - i + 1;
                }
            } else if (rep[i + 1] >= '0' && rep[i + 1] <= '9') {
                int g = rep[i + 1] - '0';
                size_t j = i + 2;
                if (j < replen && rep[j] >= '0' && rep[j] <= '9') {
                    g = g * 10 + (rep[j] - '0');
                    j++;
                }
                gnum = g;
                consumed = j - i;
            }
            if (gnum >= 0) {
                uint32_t gi = (uint32_t)gnum;
                if (gi * 2 + 1 < ovec_count * 2 && ov[gi * 2] != PCRE2_UNSET) {
                    size_t sl = (size_t)(ov[gi * 2 + 1] - ov[gi * 2]);
                    pb_put(out, subj + ov[gi * 2], sl);
                }
                i += consumed;
                continue;
            }
        }
        if (rep[i] == '\\' && i + 1 < replen) {
            if (rep[i + 1] == '\\') { pb_put(out, "\\", 1); i += 2; continue; }
            /* 其他 \x：字面保留两字符（保守；PHP 对无效转义同样原样） */
            pb_put(out, rep + i, 2);
            i += 2;
            continue;
        }
        pb_put(out, rep + i, 1);
        i++;
    }
}

/* 单 subject × 单 pattern 的替换；limit<0 或 0 = 无限。
 * 运行错误 → *hit_error=true（调用方返 null）；返回结果串（tm_malloc）。 */
static char *replace_one(VM *vm, pcre2_code *code, const char *subj,
                         size_t sublen, const char *rep, size_t replen,
                         int64_t limit, bool *hit_error, const char *fname) {
    (void)vm; (void)fname;
    pcre2_match_context *mctx = pcre2_match_context_create(NULL);
    pcre2_set_match_limit(mctx, 1000000);
    pcre2_set_depth_limit(mctx, 100000);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);
    PBuf out;
    pb_init(&out);
    PCRE2_SIZE cur = 0;
    int64_t count = 0;
    bool unlimited = (limit <= 0);
    *hit_error = false;
    while (cur <= sublen) {
        if (!unlimited && count >= limit) break;
        int rc = pcre2_match(code, (PCRE2_SPTR)subj, sublen, cur, 0, md, mctx);
        if (rc == PCRE2_ERROR_NOMATCH) break;
        if (rc < 0) {
            map_match_rc(rc);
            *hit_error = true;
            pb_free(&out);
            pcre2_match_data_free(md);
            pcre2_match_context_free(mctx);
            return NULL;
        }
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        uint32_t ovc = pcre2_get_ovector_count(md);
        PCRE2_SIZE s = ov[0], e = ov[1];
        pb_put(&out, subj + cur, (size_t)(s - cur));
        expand_replacement(&out, rep, replen, ov, ovc, subj);
        count++;
        if (s == e) {
            if (s == sublen) { cur = sublen + 1; break; }
            cur = s + 1;
        } else {
            cur = e;
        }
    }
    if (cur <= sublen) pb_put(&out, subj + cur, sublen - cur);
    pcre2_match_data_free(md);
    pcre2_match_context_free(mctx);
    if (!out.p) { out.p = tm_malloc(1); out.p[0] = 0; }
    return out.p;   /* tm_malloc；调用方负责释放 */
}

/* 把替换结果写回调用方共享字符串槽所在的数组？subject 是值——返回新串 */
TVal bi_preg_replace(VM *vm, TVal *args, int n) {
    if (n < 3 || args[0].t != T_STR || args[2].t != T_STR)
        RAISE(vm, "TypeError",
              "preg_replace() expects (string|array $pattern, string|array $replacement, string|array $subject, ...)");
    if (args[1].t != T_STR)
        RAISE(vm, "TypeError",
              "preg_replace(): replacement string required when pattern is a string (tphp v1)");
    const char *pat = tstr_val((TStr *)args[0].u.p);
    size_t patlen = tstr_len((TStr *)args[0].u.p);
    const char *rep = tstr_val((TStr *)args[1].u.p);
    size_t replen = tstr_len((TStr *)args[1].u.p);
    const char *subj = tstr_val((TStr *)args[2].u.p);
    size_t sublen = tstr_len((TStr *)args[2].u.p);
    int64_t limit = n >= 4 ? tval_to_int(args[3]) : -1;

    pcre2_code *code = compile_pattern(vm, pat, patlen, "preg_replace");
    bool err = false;
    char *res = replace_one(vm, code, subj, sublen, rep, replen, limit,
                            &err, "preg_replace");
    if (err) return TV_NULL;
    TVal out = TV_STR(tstr_new(res, strlen(res)));
    tm_free(res);
    return out;
}

TVal bi_preg_split(VM *vm, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR || args[1].t != T_STR)
        RAISE(vm, "TypeError", "preg_split() expects (string $pattern, string $subject, ...)");
    const char *pat = tstr_val((TStr *)args[0].u.p);
    size_t patlen = tstr_len((TStr *)args[0].u.p);
    const char *subj = tstr_val((TStr *)args[1].u.p);
    size_t sublen = tstr_len((TStr *)args[1].u.p);
    int64_t limit = n >= 3 ? tval_to_int(args[2]) : -1;
    int64_t flags = n >= 4 ? tval_to_int(args[3]) : 0;
    bool no_empty = (flags & PHP_PREG_SPLIT_NO_EMPTY) != 0;
    bool delim_cap = (flags & PHP_PREG_SPLIT_DELIM_CAPTURE) != 0;
    bool offset_cap = (flags & PHP_PREG_SPLIT_OFFSET_CAPTURE) != 0;

    pcre2_code *code = compile_pattern(vm, pat, patlen, "preg_split");
    pcre2_match_context *mctx = pcre2_match_context_create(NULL);
    pcre2_set_match_limit(mctx, 1000000);
    pcre2_set_depth_limit(mctx, 100000);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);

    TArr *out = tarr_new();
    PCRE2_SIZE cur = 0;
    int64_t nparts = 0;
    bool hit_err = false;
    /* limit>0：最多 limit 段（最后一段=剩余全部）；limit<=0：不限 */
    int64_t max_parts = limit > 0 ? limit : -1;
    while (cur <= sublen) {
        if (max_parts >= 0 && nparts >= max_parts - 1) break;
        int rc = pcre2_match(code, (PCRE2_SPTR)subj, sublen, cur, 0, md, mctx);
        if (rc == PCRE2_ERROR_NOMATCH) break;
        if (rc < 0) { map_match_rc(rc); hit_err = true; break; }
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        uint32_t ovc = pcre2_get_ovector_count(md);
        PCRE2_SIZE s = ov[0], e = ov[1];
        /* 分隔符前的片 */
        size_t piece_len = (size_t)(s - cur);
        if (!(no_empty && piece_len == 0)) {
            TVal item;
            if (offset_cap) {
                TArr *pair = tarr_new();
                tarr_append(pair, TV_STR(tstr_new(subj + cur, piece_len)));
                tarr_append(pair, TV_INT((int64_t)cur));
                item = TV_ARR(pair);
            } else {
                item = TV_STR(tstr_new(subj + cur, piece_len));
            }
            tarr_append(out, item);
            nparts++;
        }
        /* DELIM_CAPTURE：插入捕获组 1..n */
        if (delim_cap) {
            for (uint32_t gi = 1; gi < (uint32_t)rc; gi++) {
                uint32_t g = gi * 2;
                if (g + 1 >= ovc * 2) break;
                if (ov[g] == PCRE2_UNSET) continue;
                size_t gl = (size_t)(ov[g + 1] - ov[g]);
                if (no_empty && gl == 0) continue;
                tarr_append(out, TV_STR(tstr_new(subj + ov[g], gl)));
                nparts++;
            }
        }
        if (s == e) {
            if (s == sublen) { cur = sublen + 1; break; }
            cur = s + 1;
        } else {
            cur = e;
        }
    }
    /* 尾段 */
    if (cur <= sublen) {
        size_t tail_len = sublen - (size_t)cur;
        if (!(no_empty && tail_len == 0)) {
            TVal item;
            if (offset_cap) {
                TArr *pair = tarr_new();
                tarr_append(pair, TV_STR(tstr_new(subj + cur, tail_len)));
                tarr_append(pair, TV_INT((int64_t)cur));
                item = TV_ARR(pair);
            } else {
                item = TV_STR(tstr_new(subj + cur, tail_len));
            }
            tarr_append(out, item);
        }
    }
    pcre2_match_data_free(md);
    pcre2_match_context_free(mctx);
    (void)hit_err;   /* 运行错误：已切部分照常返回（PHP） */
    return TV_ARR(out);
}

TVal bi_preg_grep(VM *vm, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR || args[1].t != T_ARR)
        RAISE(vm, "TypeError", "preg_grep() expects (string $pattern, array $array)");
    const char *pat = tstr_val((TStr *)args[0].u.p);
    size_t patlen = tstr_len((TStr *)args[0].u.p);
    TArr *arr = (TArr *)args[1].u.p;
    int64_t flags = n >= 3 ? tval_to_int(args[2]) : 0;
    bool invert = (flags & PHP_PREG_GREP_INVERT) != 0;

    pcre2_code *code = compile_pattern(vm, pat, patlen, "preg_grep");
    pcre2_match_context *mctx = pcre2_match_context_create(NULL);
    pcre2_set_match_limit(mctx, 1000000);
    pcre2_set_depth_limit(mctx, 100000);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(code, NULL);

    TArr *out = tarr_new();
    for (uint32_t i = 0; i < arr->dused; i++) {
        if (arr->data[i].t == T_UNDEF) continue;
        if (arr->data[i].t != T_STR) continue;   /* PHP：仅字符串元素 */
        TStr *sv = (TStr *)arr->data[i].u.p;
        int rc = pcre2_match(code, (PCRE2_SPTR)sv->c, sv->len, 0, 0, md, mctx);
        bool matched = (rc >= 0);
        if (rc < 0 && rc != PCRE2_ERROR_NOMATCH) {
            map_match_rc(rc);
            break;   /* 运行错误：保留已累积结果（PHP） */
        }
        if (matched != invert) {
            /* 原键 => 元素 */
            TVal key = TV_INT((int64_t)i);   /* 简化：按下标；PHP 保留原键 */
            /* 找原键 */
            for (uint32_t b = 0; b < arr->bcap; b++) {
                if (arr->buckets[b].val_off == i && arr->buckets[b].key.t != T_NULL) {
                    key = arr->buckets[b].key;
                    break;
                }
            }
            TVal item = arr->data[i];
            tval_addref(item);
            tarr_set(out, key, item);   /* 接管 item；key 内部 addref */
        }
    }
    pcre2_match_data_free(md);
    pcre2_match_context_free(mctx);
    return TV_ARR(out);
}
