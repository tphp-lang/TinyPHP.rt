/*
 * TinyPHP.rt - a small PHP runtime
 * lex.c - hand-written tokenizer
 *
 * Notable behaviors (PHP 8 aligned):
 *  - inline HTML between ?> and <?php / <?= ; a newline right after ?> is eaten
 *  - single-line comments stop at ?> ; `#[` is an explicit error (attributes)
 *  - numbers: 0x/0o/0b, digit separators 1_000_000, floats 1.5e3/.5/5.
 *  - double-quoted strings are split into literal/expression parts
 */
#include "lex.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lex_init(Lex *lx, const char *src, const char *file) {
    lx->src = lx->pos = src;
    lx->end = src + strlen(src);
    lx->file = file;
    lx->line = 1;
    lx->in_html = true; /* file starts in HTML mode */
}

void lex_init_len(Lex *lx, const char *src, size_t len, const char *file) {
    lx->src = lx->pos = src;
    lx->end = src + len;
    lx->file = file;
    lx->line = 1;
    lx->in_html = true;
}

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

static void lex_error(Lex *lx, const char *msg) {
    tphp_fatal_src(lx->file, lx->line, "%s", msg);
}

static Token tok_simple(LKind k, int line) {
    return (Token){ .kind = k, .line = line };
}

static bool is_ident_start(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
static bool is_ident_char(unsigned char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}
static bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

/* keyword table (case-insensitive) */
static const struct { const char *name; LKind k; } keywords[] = {
    { "if", KW_IF }, { "else", KW_ELSE }, { "elseif", KW_ELSEIF },
    { "while", KW_WHILE }, { "do", KW_DO }, { "for", KW_FOR },
    { "foreach", KW_FOREACH }, { "as", KW_AS },
    { "break", KW_BREAK }, { "continue", KW_CONTINUE },
    { "switch", KW_SWITCH }, { "case", KW_CASE }, { "default", KW_DEFAULT },
    { "match", KW_MATCH }, { "return", KW_RETURN },
    { "function", KW_FUNCTION }, { "global", KW_GLOBAL }, { "unset", KW_UNSET },
    { "echo", KW_ECHO }, { "print", KW_PRINT }, { "const", KW_CONST },
    { "new", KW_NEW }, { "clone", KW_CLONE }, { "instanceof", KW_INSTANCEOF },
    { "and", KW_AND }, { "or", KW_OR }, { "xor", KW_XOR },
    { "true", KW_TRUE }, { "false", KW_FALSE }, { "null", KW_NULL },
    { "list", KW_LIST }, { "array", KW_ARRAY },
    { "try", KW_TRY }, { "catch", KW_CATCH }, { "finally", KW_FINALLY },
    { "throw", KW_THROW }, { "static", KW_STATIC },
    { "exit", KW_EXIT }, { "die", KW_DIE },
    { "isset", KW_ISSET }, { "empty", KW_EMPTY },
    { "declare", KW_DECLARE }, { "namespace", KW_NAMESPACE }, { "use", KW_USE },
    { "require", KW_REQUIRE }, { "include", KW_INCLUDE },
    { "fn", KW_FN }, { "goto", KW_GOTO }, { "class", KW_CLASS },
    /* M3 OOP */
    { "public", KW_PUBLIC }, { "protected", KW_PROTECTED }, { "private", KW_PRIVATE },
    { "abstract", KW_ABSTRACT }, { "final", KW_FINAL },
    { "interface", KW_INTERFACE }, { "trait", KW_TRAIT },
    { "extends", KW_EXTENDS }, { "implements", KW_IMPLEMENTS },
    { "readonly", KW_READONLY }, { "enum", KW_ENUM },
    { "self", KW_SELF }, { "parent", KW_PARENT }, { "insteadof", KW_INSTEADOF },
    { NULL, L_EOF },
};

static LKind ident_kind(const char *s, size_t len) {
    char buf[64];
    if (len >= sizeof buf) return L_IDENT;
    for (size_t i = 0; i < len; i++)
        buf[i] = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] + 32) : s[i];
    buf[len] = 0;
    for (int i = 0; keywords[i].name; i++)
        if (strcmp(keywords[i].name, buf) == 0) return keywords[i].k;
    return L_IDENT;
}

/* ---- HTML mode ---- */
static Token lex_html(Lex *lx) {
    int line = lx->line;
    const char *start = lx->pos;
    const char *p = lx->pos;
    while (p < lx->end) {
        if (p + 5 <= lx->end && memcmp(p, "<?php", 5) == 0 &&
            (p + 5 == lx->end || p[5] == ' ' || p[5] == '\t' || p[5] == '\n' || p[5] == '\r')) {
            break;
        }
        if (p + 3 <= lx->end && memcmp(p, "<?=", 3) == 0) break;
        if (*p == '\n') lx->line++;
        p++;
    }
    if (p == start) {
        /* adjacent open tag */
        if (p + 5 <= lx->end && memcmp(p, "<?php", 5) == 0) {
            lx->pos = p + 5;
            lx->in_html = false;
            /* skip one whitespace/newline after <?php */
            if (lx->pos < lx->end && (*lx->pos == ' ' || *lx->pos == '\t')) lx->pos++;
            else if (lx->pos < lx->end && *lx->pos == '\n') { lx->pos++; lx->line++; }
            else if (lx->pos + 1 < lx->end && lx->pos[0] == '\r' && lx->pos[1] == '\n') { lx->pos += 2; lx->line++; }
            return lex_next(lx);
        }
        lx->pos = p + 3;
        lx->in_html = false;
        return tok_simple(KW_ECHO, line); /* <?= == echo */
    }
    size_t len = (size_t)(p - start);
    lx->pos = p;
    Token t = tok_simple(L_INLINE_HTML, line);
    t.ident = tstr_intern(start, len);
    return t;
}

/* ---- double-quoted string parts ---- */
static void dq_escape_to_buf(char **out, size_t *cap, size_t *n, char c) {
    if (*n + 1 >= *cap) { *cap = *cap ? *cap * 2 : 64; *out = tm_realloc(*out, *cap); }
    (*out)[(*n)++] = c;
}

static void push_part(Token *t, LexStrPart part) {
    t->parts = tm_realloc(t->parts, sizeof(LexStrPart) * (size_t)(t->n_parts + 1));
    t->parts[t->n_parts++] = part;
}

/* shared body scanner: escapes + interpolation from p until end (or a bare
 * `"` when stop_quote). Used by double-quoted strings and heredoc.
 * Returns the scan position. */
static const char *lex_interp_body(Lex *lx, const char *p, const char *end,
                                   bool interp, bool stop_quote, Token *t) {
    char *lit = NULL; size_t lcap = 0, ln = 0;
#define LIT_FLUSH() do { \
        if (ln) { \
            push_part(t, (LexStrPart){ .is_literal = true, .u.lit = tstr_new(lit, ln) }); \
            ln = 0; \
        } \
    } while (0)
    while (p < end && !(stop_quote && *p == '"')) {
        if (interp && *p == '\\') {
            p++;
            if (p >= end) break;
            char c = *p++;
            switch (c) {
            case 'n': dq_escape_to_buf(&lit, &lcap, &ln, '\n'); break;
            case 't': dq_escape_to_buf(&lit, &lcap, &ln, '\t'); break;
            case 'r': dq_escape_to_buf(&lit, &lcap, &ln, '\r'); break;
            case 'v': dq_escape_to_buf(&lit, &lcap, &ln, '\v'); break;
            case 'f': dq_escape_to_buf(&lit, &lcap, &ln, '\f'); break;
            case 'e': dq_escape_to_buf(&lit, &lcap, &ln, 0x1b); break;
            case '\\': dq_escape_to_buf(&lit, &lcap, &ln, '\\'); break;
            case '$': dq_escape_to_buf(&lit, &lcap, &ln, '$'); break;
            case '"': dq_escape_to_buf(&lit, &lcap, &ln, '"'); break;
            case 'x': {
                unsigned v = 0; int nd = 0;
                while (nd < 2 && p < end) {
                    char h = *p;
                    int d = (h >= '0' && h <= '9') ? h - '0' :
                            (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                            (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                    if (d < 0) break;
                    v = v * 16 + (unsigned)d; p++; nd++;
                }
                dq_escape_to_buf(&lit, &lcap, &ln, (char)v);
                break;
            }
            case 'u': {
                if (p < end && *p == '{') {
                    p++;
                    unsigned v = 0;
                    while (p < end && *p != '}') {
                        char h = *p;
                        int d = (h >= '0' && h <= '9') ? h - '0' :
                                (h >= 'a' && h <= 'f') ? h - 'a' + 10 :
                                (h >= 'A' && h <= 'F') ? h - 'A' + 10 : -1;
                        if (d < 0) break;
                        v = v * 16 + (unsigned)d; p++;
                    }
                    if (p < end) p++; /* } */
                    /* encode UTF-8 */
                    if (v < 0x80) dq_escape_to_buf(&lit, &lcap, &ln, (char)v);
                    else if (v < 0x800) {
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0xC0 | (v >> 6)));
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0x80 | (v & 0x3F)));
                    } else if (v < 0x10000) {
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0xE0 | (v >> 12)));
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0x80 | ((v >> 6) & 0x3F)));
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0x80 | (v & 0x3F)));
                    } else {
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0xF0 | (v >> 18)));
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0x80 | ((v >> 12) & 0x3F)));
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0x80 | ((v >> 6) & 0x3F)));
                        dq_escape_to_buf(&lit, &lcap, &ln, (char)(0x80 | (v & 0x3F)));
                    }
                } else {
                    dq_escape_to_buf(&lit, &lcap, &ln, 'u');
                }
                break;
            }
            default:
                if (c >= '0' && c <= '7') {
                    unsigned v = (unsigned)(c - '0'); int nd = 1;
                    while (nd < 3 && p < end && *p >= '0' && *p <= '7') {
                        v = v * 8 + (unsigned)(*p - '0'); p++; nd++;
                    }
                    dq_escape_to_buf(&lit, &lcap, &ln, (char)(v & 0xFF));
                } else {
                    dq_escape_to_buf(&lit, &lcap, &ln, '\\');
                    dq_escape_to_buf(&lit, &lcap, &ln, c);
                }
            }
            if (c == '\n') lx->line++;
            continue;
        }
        /* interpolation: simple syntax $name, $name[...], $name->prop */
        if (interp && *p == '$' && p + 1 < end && (is_ident_start((unsigned char)p[1]) || p[1] == '{')) {
            const char *vs = p;
            const char *q = p + 1;
            bool brace_form = (*q == '{');
            if (brace_form) {
                /* ${var} is forbidden per PLAN/01 §9 */
                lex_error(lx, "Deprecated \"$\\{var}\" string interpolation is not supported; use {$var}");
            }
            q++;
            while (q < end && is_ident_char((unsigned char)*q)) q++;
            bool ok = true;
            if (q < end && *q == '[') {
                /* $var[key] / $var[0] : bare key or quoted key or int */
                q++;
                if (q < end && (*q == '\'' || *q == '"')) {
                    char q0 = *q++;
                    while (q < end && *q != q0) { if (*q == '\\' && q + 1 < end) q++; q++; }
                    if (q < end) q++;
                    else ok = false;
                } else {
                    while (q < end && (is_ident_char((unsigned char)*q) || *q == '-')) q++;
                }
                if (ok && q < end && *q == ']') q++;
                else ok = false;
            } else if (q < end && *q == '-' && q + 1 < end && q[1] == '>') {
                q += 2;
                while (q < end && is_ident_char((unsigned char)*q)) q++;
            }
            if (ok) {
                LIT_FLUSH();
                push_part(t, (LexStrPart){ .is_literal = false,
                            .u.expr = { .src = vs, .len = (uint32_t)(q - vs), .line = lx->line } });
                p = q;
                continue;
            }
        }
        /* complex syntax {$expr} : recurse into full expression source */
        if (interp && *p == '{' && p + 1 < end && p[1] == '$') {
            const char *vs = p + 1;
            int depth = 1;
            const char *q = p + 1;
            while (q < end && depth > 0) {
                if (*q == '{') depth++;
                else if (*q == '}') depth--;
                if (*q == '\n') lx->line++;
                if (depth) q++;
            }
            if (depth != 0) lex_error(lx, "Unterminated {$...} interpolation");
            LIT_FLUSH();
            push_part(t, (LexStrPart){ .is_literal = false,
                        .u.expr = { .src = vs, .len = (uint32_t)(q - vs), .line = lx->line } });
            p = q + 1;
            continue;
        }
        if (*p == '\n') lx->line++;
        dq_escape_to_buf(&lit, &lcap, &ln, *p);
        p++;
    }
    LIT_FLUSH();
    if (lit) tm_free(lit);
    return p;
#undef LIT_FLUSH
}

static Token lex_dq_string(Lex *lx) {
    int line = lx->line;
    Token t = tok_simple(L_STR, line);
    const char *p = lex_interp_body(lx, lx->pos, lx->end, true, true, &t);
    if (p >= lx->end) lex_error(lx, "Unterminated string literal");
    lx->pos = p + 1; /* closing quote */
    return t;
}

/* heredoc / nowdoc: <<<LABEL / <<<'LABEL' with PHP 7.3 flexible closing
 * marker (indented, code may follow on the same line) */
static Token lex_heredoc(Lex *lx) {
    int line = lx->line;
    Token t = tok_simple(L_STR, line);
    const char *p = lx->pos + 3;          /* after <<< */
    bool interp = true;
    char q0 = 0;
    if (p < lx->end && (*p == '\'' || *p == '"')) {
        interp = false;
        q0 = *p++;
        if (p >= lx->end || *p == q0) lex_error(lx, "Malformed nowdoc label");
    }
    const char *ls = p;
    while (p < lx->end && is_ident_char((unsigned char)*p)) p++;
    if (p == ls) lex_error(lx, "Heredoc label expected");
    size_t llen = (size_t)(p - ls);       /* label length (quotes excluded) */
    if (!interp) {
        /* nowdoc: closing quote required right after the label */
        if (p >= lx->end || *p != q0) lex_error(lx, "Malformed nowdoc label");
        p++;
    }
    while (p < lx->end && (*p == ' ' || *p == '\t')) p++;
    if (p >= lx->end || *p != '\n')
        lex_error(lx, "Heredoc body must start on a new line");
    p++;
    /* scan body lines for the closing marker */
    const char *body_start = p;
    const char *body_end = NULL;
    uint32_t strip = 0;
    const char *q = p;
    for (;;) {
        const char *line_start = q;
        while (q < lx->end && (*q == ' ' || *q == '\t')) q++;
        uint32_t indent = (uint32_t)(q - line_start);
        if (q + llen <= lx->end && memcmp(q, ls, llen) == 0 &&
            (q + llen >= lx->end || !is_ident_char((unsigned char)q[llen]))) {
            strip = indent;
            body_end = line_start;        /* body ends before the marker line */
            lx->pos = q + llen;           /* marker consumed; lexer continues */
            break;
        }
        while (q < lx->end && *q != '\n') q++;
        if (q >= lx->end) lex_error(lx, "Unterminated heredoc");
        q++;
    }
    if (!body_end) lex_error(lx, "Unterminated heredoc");
    /* copy body stripping `strip` leading space/tab per line; the newline
     * before the closing marker belongs to the marker (PHP semantics) */
    char *clean = tm_malloc((size_t)(body_end - body_start) + 1);
    size_t n = 0;
    for (const char *r = body_start; r < body_end; ) {
        uint32_t s = 0;
        while (r < body_end && s < strip && (*r == ' ' || *r == '\t')) { r++; s++; }
        while (r < body_end && *r != '\n') clean[n++] = *r++;
        if (r < body_end) { clean[n++] = '\n'; r++; }
    }
    if (n && clean[n - 1] == '\n') n--;
    if (interp) {
        lex_interp_body(lx, clean, clean + n, true, false, &t);
        /* ownership of `clean` moves into the parts' expr.src pointers
         * (still referenced at parse time); intentionally not freed */
    } else {
        for (size_t i = 0; i < n; i++) if (clean[i] == '\n') lx->line++;
        if (n) push_part(&t, (LexStrPart){ .is_literal = true, .u.lit = tstr_new(clean, n) });
        tm_free(clean);
    }
    (void)line;
    return t;
}

static Token lex_single_quote(Lex *lx) {
    int line = lx->line;
    Token t = tok_simple(L_STR, line);
    char *lit = NULL; size_t lcap = 0, ln = 0;
    const char *p = lx->pos;
    while (p < lx->end && *p != '\'') {
        if (*p == '\\' && p + 1 < lx->end && (p[1] == '\'' || p[1] == '\\')) {
            dq_escape_to_buf(&lit, &lcap, &ln, p[1]);
            p += 2;
            continue;
        }
        if (*p == '\n') lx->line++;
        dq_escape_to_buf(&lit, &lcap, &ln, *p);
        p++;
    }
    if (p >= lx->end) lex_error(lx, "Unterminated string literal");
    lx->pos = p + 1;
    if (!lit) return t; /* empty string: no parts */
    push_part(&t, (LexStrPart){ .is_literal = true, .u.lit = tstr_new(lit, ln) });
    tm_free(lit);
    return t;
}

/* scan a number; returns token */
static Token lex_number(Lex *lx) {
    int line = lx->line;
    const char *p = lx->pos;
    bool is_float = false;
    /* base-prefixed integers: 0x 0b 0o (overflow -> float) */
    struct { const char *prefix; int base; } bases[] = {
        { "xX", 16 }, { "bB", 2 }, { "oO", 8 },
    };
    for (size_t bi = 0; bi < 3; bi++) {
        if (p + 1 < lx->end && p[0] == '0' && strchr(bases[bi].prefix, p[1])) {
            int base = bases[bi].base;
            p += 2;
            char buf[256]; size_t n = 0;
            while (p < lx->end) {
                if (*p == '_') { p++; continue; } /* digit separator */
                int d = -1;
                if (*p >= '0' && *p <= '9') d = *p - '0';
                else if (base == 16) {
                    int l = (*p | 32);
                    if (l >= 'a' && l <= 'f') d = l - 'a' + 10;
                }
                if (d < 0 || d >= base) break;
                if (n < sizeof buf - 1)
                    buf[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
                p++;
            }
            lx->pos = p;
            buf[n] = 0;
            errno = 0;
            char *endp = NULL;
            long long v = strtoll(buf, &endp, base);
            if (errno == ERANGE || (n > 0 && endp && *endp)) {
                double dv = 0; /* overflow: double accumulation */
                for (size_t k = 0; k < n; k++) {
                    int d = (buf[k] >= '0' && buf[k] <= '9') ? buf[k] - '0' : buf[k] - 'a' + 10;
                    dv = dv * base + (double)d;
                }
                Token t = tok_simple(L_FLOAT, line);
                t.d = dv;
                return t;
            }
            Token t = tok_simple(L_INT, line);
            t.i = (int64_t)v;
            return t;
        }
    }
    /* decimal / float */
    const char *start = p;
    while (p < lx->end && (is_digit((unsigned char)*p) || *p == '_')) p++;
    if (p < lx->end && *p == '.' && p + 1 < lx->end && is_digit((unsigned char)p[1])) {
        is_float = true;
        p++;
        while (p < lx->end && (is_digit((unsigned char)*p) || *p == '_')) p++;
    } else if (p < lx->end && *p == '.' && p > start && is_digit((unsigned char)p[-1])) {
        /* trailing dot: "5." */
        is_float = true;
        p++;
    }
    /* exponent: valid after int (1e3) and float (1.0e15) mantissas alike */
    if (p < lx->end && (*p == 'e' || *p == 'E')) {
        const char *save = p;
        p++;
        if (p < lx->end && (*p == '+' || *p == '-')) p++;
        if (p < lx->end && is_digit((unsigned char)*p)) {
            is_float = true;
            while (p < lx->end && (is_digit((unsigned char)*p) || *p == '_')) p++;
        } else {
            p = save;
        }
    }
    char buf[512];
    size_t n = (size_t)(p - start);
    if (n >= sizeof buf) lex_error(lx, "Numeric literal too long");
    size_t j = 0;
    for (size_t k = 0; k < n; k++) if (start[k] != '_') buf[j++] = start[k];
    buf[j] = 0;
    lx->pos = p;
    if (is_float) {
        Token t = tok_simple(L_FLOAT, line);
        t.d = strtod(buf, NULL);
        return t;
    }
    /* int, overflow -> float (PHP) */
    errno = 0;
    char *endp = NULL;
    long long v = strtoll(buf, &endp, 10);
    if (errno == ERANGE) {
        Token t = tok_simple(L_FLOAT, line);
        t.d = strtod(buf, NULL);
        return t;
    }
    Token t = tok_simple(L_INT, line);
    t.i = (int64_t)v;
    return t;
}

static bool match2(Lex *lx, char c0, char c1) {
    if (lx->pos + 1 < lx->end && lx->pos[0] == c0 && lx->pos[1] == c1) {
        lx->pos += 2;
        return true;
    }
    return false;
}

Token lex_next(Lex *lx) {
    if (lx->in_html) return lex_html(lx);
    int line = lx->line;
    /* skip whitespace and comments */
    for (;;) {
        char c = lx->pos < lx->end ? *lx->pos : 0;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') { lx->pos++; continue; }
        if (c == '\n') { lx->line++; lx->pos++; continue; }
        if (c == '/' && lx->pos + 1 < lx->end && lx->pos[1] == '/') {
            lx->pos += 2;
            while (lx->pos < lx->end && *lx->pos != '\n') {
                /* single-line comment ends at ?> too */
                if (lx->pos + 2 <= lx->end && lx->pos[0] == '?' && lx->pos[1] == '>') break;
                lx->pos++;
            }
            continue;
        }
        if (c == '#') {
            if (lx->pos + 1 < lx->end && lx->pos[1] == '[')
                lex_error(lx, "Attributes (#[...]) are not supported");
            lx->pos++;
            while (lx->pos < lx->end && *lx->pos != '\n') {
                if (lx->pos + 2 <= lx->end && lx->pos[0] == '?' && lx->pos[1] == '>') break;
                lx->pos++;
            }
            continue;
        }
        if (c == '/' && lx->pos + 1 < lx->end && lx->pos[1] == '*') {
            lx->pos += 2;
            while (lx->pos + 1 < lx->end && !(lx->pos[0] == '*' && lx->pos[1] == '/')) {
                if (*lx->pos == '\n') lx->line++;
                lx->pos++;
            }
            lx->pos = lx->pos + 2 <= lx->end ? lx->pos + 2 : lx->end;
            continue;
        }
        /* close tag: switch to HTML mode, swallow one newline */
        if (c == '?' && lx->pos + 1 < lx->end && lx->pos[1] == '>') {
            lx->pos += 2;
            if (lx->pos + 1 < lx->end && lx->pos[0] == '\r' && lx->pos[1] == '\n') lx->pos += 2;
            else if (lx->pos < lx->end && lx->pos[0] == '\n') lx->pos++;
            lx->line++;
            lx->in_html = true;
            return lex_next(lx);
        }
        break;
    }
    if (lx->pos >= lx->end) return tok_simple(L_EOF, line);

    char c = *lx->pos;
    /* variables */
    if (c == '$') {
        lx->pos++;
        const char *s = lx->pos;
        while (lx->pos < lx->end && is_ident_char((unsigned char)*lx->pos)) lx->pos++;
        if (lx->pos == s) lex_error(lx, "Expected variable name after '$'");
        Token t = tok_simple(L_VAR, line);
        t.ident = tstr_intern(s, (size_t)(lx->pos - s));
        return t;
    }
    if (is_ident_start((unsigned char)c)) {
        const char *s = lx->pos;
        while (lx->pos < lx->end && is_ident_char((unsigned char)*lx->pos)) lx->pos++;
        Token t = tok_simple(ident_kind(s, (size_t)(lx->pos - s)), line);
        t.ident = tstr_intern(s, (size_t)(lx->pos - s));
        return t;
    }
    if (is_digit((unsigned char)c) || (c == '.' && lx->pos + 1 < lx->end && is_digit((unsigned char)lx->pos[1])))
        return lex_number(lx);
    if (c == '\'') { lx->pos++; return lex_single_quote(lx); }
    if (c == '"') { lx->pos++; return lex_dq_string(lx); }

    /* punctuation & operators */
    lx->pos++;
    switch (c) {
    case '(': return tok_simple(P_LPAREN, line);
    case ')': return tok_simple(P_RPAREN, line);
    case '[': return tok_simple(P_LBRACKET, line);
    case ']': return tok_simple(P_RBRACKET, line);
    case '{': return tok_simple(P_LBRACE, line);
    case '}': return tok_simple(P_RBRACE, line);
    case ',': return tok_simple(P_COMMA, line);
    case ';': return tok_simple(P_SEMI, line);
    case '\\': return tok_simple(P_BACKSLASH, line);
    case '@': return tok_simple(P_AT, line);
    case ':':
        /* first ':' consumed; '::' when this char is another ':' */
        if (lx->pos < lx->end && *lx->pos == ':') { lx->pos++; return tok_simple(P_DOUBLE_COLON, line); }
        return tok_simple(P_COLON, line);
    case '.':
        if (match2(lx, '.', '.')) return tok_simple(P_ELLIPSIS, line);
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_DOT_EQ, line); }
        return tok_simple(P_DOT, line);
    case '+':
        if (lx->pos < lx->end && *lx->pos == '+') { lx->pos++; return tok_simple(P_INC, line); }
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_PLUS_EQ, line); }
        return tok_simple(P_PLUS, line);
    case '-':
        if (lx->pos < lx->end && *lx->pos == '-') { lx->pos++; return tok_simple(P_DEC, line); }
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_MINUS_EQ, line); }
        if (lx->pos < lx->end && *lx->pos == '>') { lx->pos++; return tok_simple(P_OBJECT, line); }
        return tok_simple(P_MINUS, line);
    case '*':
        if (match2(lx, '*', '=')) return tok_simple(P_POW_EQ, line);
        if (lx->pos < lx->end && *lx->pos == '*') { lx->pos++; return tok_simple(P_POW, line); }
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_MUL_EQ, line); }
        return tok_simple(P_MUL, line);
    case '/':
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_DIV_EQ, line); }
        return tok_simple(P_DIV, line);
    case '%':
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_MOD_EQ, line); }
        return tok_simple(P_MOD, line);
    case '=':
        if (match2(lx, '=', '=')) return tok_simple(P_SEQ, line);
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_EQ, line); }
        if (lx->pos < lx->end && *lx->pos == '>') { lx->pos++; return tok_simple(P_FATARROW, line); }
        return tok_simple(P_ASSIGN, line);
    case '!':
        if (match2(lx, '=', '=')) return tok_simple(P_SNE, line);
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_NE, line); }
        return tok_simple(P_NOT, line);
    case '<':
        if (match2(lx, '=', '>')) return tok_simple(P_SPACESHIP, line);
        if (match2(lx, '<', '=')) return tok_simple(P_SHL_EQ, line);
        if (lx->pos < lx->end && *lx->pos == '<') {
            /* heredoc/nowdoc: <<<LABEL or <<<'LABEL' (pos is at the 2nd '<') */
            if (lx->pos + 1 < lx->end && lx->pos[1] == '<' && lx->pos + 2 < lx->end) {
                char c1 = lx->pos[2];
                if (is_ident_start((unsigned char)c1) || c1 == '\'' || c1 == '"') {
                    lx->pos--;        /* back to the 1st '<': lex_heredoc skips <<< */
                    return lex_heredoc(lx);
                }
            }
            lx->pos++;
            return tok_simple(P_SHL, line);
        }
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_LE, line); }
        if (lx->pos < lx->end && *lx->pos == '>') { lx->pos++; return tok_simple(P_NE, line); }
        return tok_simple(P_LT, line);    case '>':
        if (match2(lx, '>', '=')) return tok_simple(P_SHR_EQ, line);
        if (lx->pos < lx->end && *lx->pos == '>') { lx->pos++; return tok_simple(P_SHR, line); }
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_GE, line); }
        return tok_simple(P_GT, line);
    case '&':
        if (match2(lx, '&', '=')) return tok_simple(P_AND_EQ, line);
        if (lx->pos < lx->end && *lx->pos == '&') { lx->pos++; return tok_simple(P_ANDAND, line); }
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_AND_EQ, line); }
        return tok_simple(P_AMP, line);
    case '|':
        if (lx->pos < lx->end && *lx->pos == '>') { lx->pos++; return tok_simple(P_PIPELINE, line); }
        if (match2(lx, '|', '=')) return tok_simple(P_OR_EQ, line);
        if (lx->pos < lx->end && *lx->pos == '|') { lx->pos++; return tok_simple(P_OROR, line); }
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_OR_EQ, line); }
        return tok_simple(P_PIPE, line);
    case '^':
        if (lx->pos < lx->end && *lx->pos == '=') { lx->pos++; return tok_simple(P_XOR_EQ, line); }
        return tok_simple(P_CARET, line);
    case '~': return tok_simple(P_TILDE, line);
    case '?':
        if (match2(lx, '?', '=')) return tok_simple(P_COALESCE_EQ, line);
        if (lx->pos < lx->end && *lx->pos == '?') { lx->pos++; return tok_simple(P_COALESCE, line); }
        if (lx->pos < lx->end && *lx->pos == '-') { lx->pos++; return tok_simple(P_NULLSAFE, line); }
        return tok_simple(P_QUESTION, line);
    default: break;
    }
    lex_error(lx, "Unexpected character");
}
