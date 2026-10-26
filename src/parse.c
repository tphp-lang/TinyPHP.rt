/*
 * TinyPHP.rt - a small PHP runtime
 * parse.c - recursive-descent parser (priority-chain driven, PHP 8 precedence)
 *
 * M1 scope: procedural statements, top-level functions (position args +
 * constant defaults), arrays with full key semantics, destructuring
 * (list()/[]), spread, global, full operators, interpolation.
 * Unsupported M2/M3 constructs raise a clear load-time error.
 */
#include "ast.h"
#include "lex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
/* windows.h would drag in a POSIX accept() declaration that collides with
 * parse.c's own accept(); _fullpath in <stdlib.h> is enough here. */
#include <direct.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));


typedef struct {
    Lex lx;
    Token tok;       /* 1-token lookahead */
    const char *file;
    TStr *ns;        /* current namespace (S_NS_END carries the outer) */
    int in_const_expr; /* 默认值/类常量/属性默认上下文（yield 禁止） */
    /* 函数上下文栈：yield 标记栈顶 AFunc（parse 期，PHP 同款）；
     * 空栈 = 函数体外（加载期拒绝） */
    struct AFunc *fn_stack[64];
    int n_fn_stack;
} Parser;
/* PHP 8.4 __PROPERTY__：钩子体内折叠为属性名字符串（parse 期设置） */
static TStr *g_hook_prop_name;
static AExpr *parse_unary(Parser *p);
static AExpr *parse_closure(Parser *p, bool is_static);
static AExpr *parse_arrow_fn(Parser *p);


static AExpr *parse_expr(Parser *p);
static AExpr *parse_ternary(Parser *p);
static AStmt *parse_statement(Parser *p);
static AExpr *parse_static_access(Parser *p, TStr *name, int line);
static TStr **parse_name_list(Parser *p, int *out_n);
static AClassBody *parse_class_body(Parser *p, int kind, TStr *cls_name);

/* ---- small helpers ---- */
static void p_error(Parser *p, const char *msg)
    __attribute__((noreturn));

static void p_error(Parser *p, const char *msg) {
    tphp_fatal_src(p->file, p->tok.line, "Parse error: %s", msg);
}

static void advance(Parser *p) { p->tok = lex_next(&p->lx); }

static bool accept(Parser *p, LKind k) {
    if (p->tok.kind == k) { advance(p); return true; }
    return false;
}

static void expect(Parser *p, LKind k, const char *what) {
    if (p->tok.kind != k) p_error(p, what);
    advance(p);
}

/* qualified name: [\] IDENT (\ IDENT)* — one TStr, leading '\' = absolute.
 * `first` = already-consumed leading IDENT (callers that advanced past it),
 * or NULL to read the first segment (and the optional leading '\') here. */
static TStr *parse_qual_name(Parser *p, TStr *first) {
    bool abs = false;
    const char *s0;
    if (first) {
        s0 = tstr_val(first);
    } else {
        if (p->tok.kind == P_BACKSLASH) { abs = true; advance(p); }
        if (p->tok.kind != L_IDENT) p_error(p, "Expected name");
        s0 = tstr_val(p->tok.ident);
        advance(p);
    }
    size_t cap0 = strlen(s0) + abs + 16;
    char *buf = tm_malloc(cap0);
    size_t n = 0;
    if (abs) buf[n++] = '\\';
    memcpy(buf + n, s0, strlen(s0)); n += strlen(s0);
    while (p->tok.kind == P_BACKSLASH) {
        advance(p);
        if (p->tok.kind != L_IDENT) p_error(p, "Expected name after '\\'");
        const char *seg = tstr_val(p->tok.ident);
        size_t need = n + strlen(seg) + 2;
        if (need > cap0) { cap0 = need * 2; buf = tm_realloc(buf, cap0); }
        buf[n++] = '\\';
        memcpy(buf + n, seg, strlen(seg)); n += strlen(seg);
        advance(p);
    }
    TStr *out = tstr_intern(buf, n);
    tm_free(buf);
    return out;
}

/* use 语句的限定名前缀：与 parse_qual_name 同形，但允许以 `\{` 结尾
 * （组 use：use A\B\{C, D as E};）。*group 置位时当前 token 是 '{'，
 * 返回的前缀不含末尾反斜杠。 */
static TStr *parse_use_prefix(Parser *p, bool *group) {
    bool abs = false;
    *group = false;
    if (p->tok.kind == P_BACKSLASH) { abs = true; advance(p); }
    if (p->tok.kind != L_IDENT) p_error(p, "Expected name");
    const char *s0 = tstr_val(p->tok.ident);
    size_t cap = strlen(s0) + (size_t)abs + 16;
    char *buf = tm_malloc(cap);
    size_t n = 0;
    if (abs) buf[n++] = '\\';
    memcpy(buf + n, s0, strlen(s0));
    n += strlen(s0);
    advance(p);
    while (p->tok.kind == P_BACKSLASH) {
        advance(p);
        if (p->tok.kind == P_LBRACE) {          /* A\B\{ ... } */
            *group = true;
            break;
        }
        if (p->tok.kind != L_IDENT) p_error(p, "Expected name after '\\'");
        const char *seg = tstr_val(p->tok.ident);
        size_t need = n + strlen(seg) + 2;
        if (need > cap) { cap = need * 2; buf = tm_realloc(buf, cap); }
        buf[n++] = '\\';
        memcpy(buf + n, seg, strlen(seg));
        n += strlen(seg);
        advance(p);
    }
    TStr *out = tstr_intern(buf, n);
    tm_free(buf);
    return out;
}

static TStr *expect_ident(Parser *p, const char *what) {
    if (p->tok.kind != L_IDENT) p_error(p, what);
    TStr *name = p->tok.ident;
    advance(p);
    return name;
}

/* 成员名位置（`->` / `?->` 之后）：标识符或任意关键字。
 * PHP 允许 $o->fn / $o->match / $o->class —— 关键字在成员位置退化为
 * 标识符（半保留字含 yield/from：enum E::from、方法名 from 均合法）。
 * lex.c 对所有标识符（含关键字 token）都填了 ident，可直接取用。 */
static bool tok_is_member_name(LKind k) {
    return k == L_IDENT || (k >= KW_IF && k <= KW_FROM);
}

static AExpr *ae_new(int tag, int line) {
    AExpr *e = tm_malloc(sizeof(AExpr));
    memset(e, 0, sizeof *e);
    e->tag = tag;
    e->line = line;
    return e;
}

/* 常量折叠：`__DIR__ . '/x.php'` 这类纯字面量拼接 → 单个 A_STR。
 * 目的：让 require/include 走编译期内联。否则运行期 OP_REQUIRE 会为被
 * 包含文件生成独立 Program（inc_progs），其中的闭包/函数无法作为
 * Worker 入口跨线程共享。__DIR__/__FILE__ 在 parse 阶段已折叠成 A_STR，
 * 所以这里只需处理字符串的 `.` 拼接。 */
static AExpr *fold_str_concat(AExpr *e) {
    if (!e || e->tag != A_BINARY || e->u.bin.op != P_DOT) return e;
    AExpr *l = fold_str_concat(e->u.bin.l);
    AExpr *r = fold_str_concat(e->u.bin.r);
    if (l->tag != A_STR || r->tag != A_STR) {
        e->u.bin.l = l;
        e->u.bin.r = r;
        return e;
    }
    const char *ls = tstr_val(l->u.name);
    const char *rs = tstr_val(r->u.name);
    size_t ln = strlen(ls), rn = strlen(rs);
    char *buf = tm_malloc(ln + rn + 1);
    memcpy(buf, ls, ln);
    memcpy(buf + ln, rs, rn);
    buf[ln + rn] = 0;
    AExpr *out = ae_new(A_STR, e->line);
    out->u.name = tstr_intern(buf, ln + rn);
    tm_free(buf);
    return out;
}

/* async(EXPR) 语法结构：当唯一实参是"调用表达式"时，把整个调用搬进
 * 协程执行（改写为 Tphp\spawn(fn() => EXPR)）。箭头函数自动按值捕获
 * 自由变量，所以参数里的变量在 spawn 那一刻取值。
 * 单实参但不是调用形态时（async($callable)）按普通函数调用处理。 */
static AStmt *as_new(int tag, int line);
static AStmtList *asl_new(void);
static void asl_push(AStmtList *l, AStmt *s);

static bool ast_is_call_expr(const AExpr *e) {
    return e->tag == A_CALL || e->tag == A_METHODCALL || e->tag == A_STATICCALL;
}

static AExpr *make_async_thunk(AExpr *expr, int line) {
    AFunc *f = tm_malloc(sizeof(AFunc));
    memset(f, 0, sizeof *f);
    f->line = line;
    AStmtList *body = asl_new();
    AStmt *ret = as_new(S_RETURN, line);
    ret->u.e = expr;
    asl_push(body, ret);
    f->body = body;

    AExpr *arrow = ae_new(A_ARROW, line);
    arrow->u.clo.func = f;
    return arrow;
}

/* 把调用表达式包成 `fn() => EXPR`（spawn 类方法用，保留原调用） */
static AExpr *wrap_thunk(AExpr *expr, int line) {
    return make_async_thunk(expr, line);
}

/* async(EXPR) → Tphp\spawn(fn() => EXPR) */
static AExpr *make_async_call(AExpr *expr, int line) {
    AExpr *arrow = make_async_thunk(expr, line);

    AArgList *args = tm_malloc(sizeof(AArgList));
    args->items = tm_malloc(sizeof(AArg));
    args->items[0].e = arrow;
    args->items[0].name = NULL;
    args->items[0].is_spread = false;
    args->n = 1;

    AExpr *call = ae_new(A_CALL, line);
    call->u.call.name = tstr_intern("\\Tphp\\spawn", 11);
    call->u.call.args = args;
    return call;
}

/* dirname for __DIR__: strip at last '/' or '\\'; returns NULL at fs root
 * or for a bare name (no separator). */
static const char *parse_dirname(const char *path, size_t *out_len) {
    const char *last = NULL;
    for (const char *q = path; *q; q++)
        if (*q == '/' || *q == '\\') last = q;
    if (!last) return NULL;
    *out_len = (size_t)(last - path);
    if (*out_len == 0) *out_len = 1;   /* "/x" -> "/" */
    return path;
}

/* Resolve to an absolute, separator-normalized path. PHP's __FILE__ is always
 * absolute; we need that too so a runtime cwd change cannot break the io
 * layer's source_root -> bundle key mapping (T12/T13). Best-effort: if the
 * platform call fails we fall back to the input unchanged. */
static void parse_abspath(const char *in, char *out, size_t cap) {
    out[0] = 0;
    if (!in || !in[0]) return;
#ifdef _WIN32
    char full[1024];
    if (_fullpath(full, in, sizeof full) == NULL) { snprintf(out, cap, "%s", in); }
    else { snprintf(out, cap, "%s", full); }
#else
    char full[PATH_MAX];
    if (!realpath(in, full)) { snprintf(out, cap, "%s", in); }
    else { snprintf(out, cap, "%s", full); }
#endif
    /* normalize backslashes to '/' so zip keys and source_root agree */
    for (char *q = out; *q; q++) if (*q == '\\') *q = '/';
}

static AStmt *as_new(int tag, int line) {
    AStmt *s = tm_malloc(sizeof(AStmt));
    memset(s, 0, sizeof *s);
    s->tag = tag;
    s->line = line;
    return s;
}

static AExprList *ael_new(void) {
    AExprList *l = tm_malloc(sizeof(AExprList));
    l->items = NULL; l->n = 0;
    return l;
}

static void ael_push(AExprList *l, AExpr *e) {
    l->items = tm_realloc(l->items, sizeof(AExpr *) * (size_t)(l->n + 1));
    l->items[l->n++] = e;
}

static AStmtList *asl_new(void) {
    AStmtList *l = tm_malloc(sizeof(AStmtList));
    l->stmts = NULL; l->n = 0;
    return l;
}

static void asl_push(AStmtList *l, AStmt *s) {
    l->stmts = tm_realloc(l->stmts, sizeof(AStmt *) * (size_t)(l->n + 1));
    l->stmts[l->n++] = s;
}

static AStmtList *asl_wrap(AStmt *s) {
    AStmtList *l = asl_new();
    asl_push(l, s);
    return l;
}

/* ============ interpolation: string parts -> concat chain ============ */
static AExpr *parse_sub_expression(Parser *p, const char *src, uint32_t len, int line) {
    Lex saved = p->lx;
    Token saved_tok = p->tok;
    lex_init_len(&p->lx, src, len, p->file);   /* part is not NUL-terminated */
    p->lx.in_html = false;
    p->lx.line = line;
    advance(p);
    AExpr *e = parse_expr(p);
    if (p->tok.kind != L_EOF)
        p_error(p, "Unexpected token in interpolated expression");
    p->lx = saved;
    p->tok = saved_tok;
    return e;
}

static AExpr *build_string(Parser *p, Token *t) {
    if (t->n_parts == 0) {
        AExpr *e = ae_new(A_STR, t->line);
        e->u.name = tstr_intern("", 0);     /* empty literal "" */
        return e;
    }
    if (t->n_parts == 1 && t->parts[0].is_literal) {
        AExpr *e = ae_new(A_STR, t->line);
        e->u.name = t->parts[0].u.lit;
        return e;
    }
    AExpr *acc = NULL;
    for (int i = 0; i < t->n_parts; i++) {
        AExpr *part;
        if (t->parts[i].is_literal) {
            part = ae_new(A_STR, t->line);
            part->u.name = t->parts[i].u.lit;
        } else {
            part = parse_sub_expression(p, t->parts[i].u.expr.src,
                                        t->parts[i].u.expr.len,
                                        t->parts[i].u.expr.line);
        }
        if (!acc) { acc = part; continue; }
        AExpr *cat = ae_new(A_BINARY, t->line);
        cat->u.bin.op = P_DOT;
        cat->u.bin.l = acc;
        cat->u.bin.r = part;
        acc = cat;
    }
    return acc;
}

/* ============ calls / arrays / match / isset ============ */
static AArgList *parse_arg_list(Parser *p) {
    expect(p, P_LPAREN, "Expected '('");
    AArgList *al = tm_malloc(sizeof(AArgList));
    al->items = NULL; al->n = 0;
    if (p->tok.kind != P_RPAREN) {
        for (;;) {
            AArg arg;
            memset(&arg, 0, sizeof arg);
            if (accept(p, P_ELLIPSIS)) {
                arg.is_spread = true;
                arg.e = parse_expr(p);
            } else {
                /* named argument lookahead: IDENT ':' */
                if (p->tok.kind == L_IDENT) {
                    Lex saved = p->lx;
                    Token saved_tok = p->tok;
                    advance(p);
                    if (p->tok.kind == P_COLON) {
                        arg.name = saved_tok.ident;
                        advance(p);
                    } else {
                        p->lx = saved;
                        p->tok = saved_tok;
                    }
                }
                arg.e = parse_expr(p);
            }
            al->items = tm_realloc(al->items, sizeof(AArg) * (size_t)(al->n + 1));
            al->items[al->n++] = arg;
            if (!accept(p, P_COMMA)) break;
            if (p->tok.kind == P_RPAREN) break; /* trailing comma */
        }
    }
    expect(p, P_RPAREN, "Expected ')'");
    return al;
}

static AExpr *parse_array_literal(Parser *p, bool bracket_form) {
    int line = p->tok.line;
    advance(p); /* '[' or array keyword */
    if (!bracket_form) expect(p, P_LPAREN, "Expected '(' after array");
    AArrayList *al = tm_malloc(sizeof(AArrayList));
    al->items = NULL; al->n = 0;
    LKind closer = bracket_form ? P_RBRACKET : P_RPAREN;
    if (p->tok.kind != closer) {
        for (;;) {
            AArrayItem item = { .key = NULL, .val = NULL, .is_spread = false };
            if (accept(p, P_ELLIPSIS)) {
                item.is_spread = true;
                item.val = parse_expr(p);
            } else {
                item.val = parse_expr(p);
                if (accept(p, P_FATARROW)) {
                    item.key = item.val;
                    item.val = parse_expr(p);
                }
            }
            al->items = tm_realloc(al->items, sizeof(AArrayItem) * (size_t)(al->n + 1));
            al->items[al->n++] = item;
            if (!accept(p, P_COMMA)) break;
            if (p->tok.kind == closer) break; /* trailing comma */
        }
    }
    expect(p, closer, bracket_form ? "Expected ']'" : "Expected ')'");
    AExpr *e = ae_new(A_ARRAY, line);
    e->u.alist = al;
    return e;
}

static AExpr *parse_match(Parser *p) {
    int line = p->tok.line;
    advance(p); /* match */
    expect(p, P_LPAREN, "Expected '(' after match");
    AExpr *cond = parse_expr(p);
    expect(p, P_RPAREN, "Expected ')'");
    expect(p, P_LBRACE, "Expected '{' after match(...)");
    AMatchList *arms = tm_malloc(sizeof(AMatchList));
    arms->arms = NULL; arms->n = 0;
    while (p->tok.kind != P_RBRACE) {
        AMatchArm arm = { .conds = NULL, .n_conds = 0, .body = NULL, .is_default = false };
        if (accept(p, KW_DEFAULT)) {
            arm.is_default = true;
        } else {
            for (;;) {
                AExpr *c = parse_expr(p);
                arm.conds = tm_realloc(arm.conds, sizeof(AExpr *) * (size_t)(arm.n_conds + 1));
                arm.conds[arm.n_conds++] = c;
                if (!accept(p, P_COMMA)) break;
            }
        }
        expect(p, P_FATARROW, "Expected '=>' in match arm");
        arm.body = parse_expr(p);
        arms->arms = tm_realloc(arms->arms, sizeof(AMatchArm) * (size_t)(arms->n + 1));
        arms->arms[arms->n++] = arm;
        if (!accept(p, P_COMMA)) break;
    }
    expect(p, P_RBRACE, "Expected '}' to close match");
    AExpr *e = ae_new(A_MATCH, line);
    e->u.match.cond = cond;
    e->u.match.arms = arms;
    return e;
}

static AExpr *parse_primary(Parser *p) {
    int line = p->tok.line;
    switch (p->tok.kind) {
    case L_INT: {
        AExpr *e = ae_new(A_INT, line);
        e->u.i = p->tok.i;
        advance(p);
        return e;
    }
    case L_FLOAT: {
        AExpr *e = ae_new(A_FLOAT, line);
        e->u.d = p->tok.d;
        advance(p);
        return e;
    }
    case L_STR: {
        Token t = p->tok;
        advance(p);
        return build_string(p, &t);
    }
    case L_INLINE_HTML: {
        AExpr *e = ae_new(A_STR, line);
        e->u.name = p->tok.ident;
        advance(p);
        return e;
    }
    case KW_TRUE: advance(p); return ae_new(A_TRUE, line);
    case KW_FALSE: advance(p); return ae_new(A_FALSE, line);
    case KW_NULL: advance(p); return ae_new(A_NULL, line);
    case KW_REQUIRE: case KW_INCLUDE:
    case KW_REQUIRE_ONCE: case KW_INCLUDE_ONCE: {
        /* 表达式形态：$v = require 'x'; / require $path;
         * 字面量路径编译期内联；动态路径 OP_REQUIRE 运行期求值 */
        bool is_req = p->tok.kind == KW_REQUIRE || p->tok.kind == KW_REQUIRE_ONCE;
        bool is_once = p->tok.kind == KW_REQUIRE_ONCE || p->tok.kind == KW_INCLUDE_ONCE;
        advance(p);
        AExpr *path = fold_str_concat(parse_ternary(p));
        AExpr *e = ae_new(A_REQUIRE, line);
        e->u.inc.is_require = is_req;
        e->u.inc.is_once = is_once;
        if (path->tag == A_STR) {
            e->u.inc.path = path->u.name;
            e->u.inc.path_expr = NULL;
        } else {
            e->u.inc.path = NULL;
            e->u.inc.path_expr = path;   /* 运行期求值 */
        }
        return e;
    }
    case L_VAR: {
        /* C1: `$$v` 只在成员上下文（->$v / ::$$v）合法；普通表达式仍是
         * 变量变量（不支持），保持与原词法错误同等的明确报错 */
        if (tstr_val(p->tok.ident)[0] == '$')
            p_error(p, "Variable-variable ($$var) is not supported");
        AExpr *e = ae_new(A_VAR, line);
        e->u.name = p->tok.ident;
        advance(p);
        return e;
    }
    case P_LPAREN: {
        advance(p);
        AExpr *e = parse_expr(p);
        expect(p, P_RPAREN, "Expected ')'");
        return e;
    }
    case P_LBRACKET:
        return parse_array_literal(p, true);
    case KW_ARRAY:
        return parse_array_literal(p, false);
    case KW_MATCH:
        return parse_match(p);
    case KW_ISSET: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after isset");
        AExprList *args = ael_new();
        while (p->tok.kind != P_RPAREN) {
            ael_push(args, parse_expr(p));
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_RPAREN, "Expected ')'");
        AExpr *e = ae_new(A_ISSET, line);
        e->u.args = args;
        return e;
    }
    case KW_EMPTY: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after empty");
        AExprList *args = ael_new();
        ael_push(args, parse_expr(p));
        expect(p, P_RPAREN, "Expected ')'");
        AExpr *e = ae_new(A_EMPTY, line);
        e->u.args = args;
        return e;
    }
    case KW_PRINT: {
        advance(p);
        AExpr *arg = parse_ternary(p);
        AArgList *args = tm_malloc(sizeof(AArgList));
        args->items = tm_malloc(sizeof(AArg));
        args->items[0].e = arg;
        args->items[0].name = NULL;
        args->items[0].is_spread = false;
        args->n = 1;
        AExpr *e = ae_new(A_CALL, line);
        e->u.call.name = tstr_intern("print", 5);
        e->u.call.args = args;
        return e;
    }
    case KW_EXIT:
    case KW_DIE: {
        advance(p);
        AArgList *args = tm_malloc(sizeof(AArgList));
        args->items = NULL; args->n = 0;
        if (p->tok.kind == P_LPAREN) {
            advance(p);
            if (p->tok.kind != P_RPAREN) {
                args->items = tm_malloc(sizeof(AArg));
                args->items[0].e = parse_expr(p);
                args->items[0].name = NULL;
                args->items[0].is_spread = false;
                args->n = 1;
            }
            expect(p, P_RPAREN, "Expected ')'");
        }
        AExpr *e = ae_new(A_CALL, line);
        e->u.call.name = tstr_intern("exit", 4);
        e->u.call.args = args;
        return e;
    }
    case L_IDENT: {
        TStr *name = p->tok.ident;
        advance(p);
        /* qualified name: IDENT (\ IDENT)* (functions/constants/classes) */
        if (p->tok.kind == P_BACKSLASH)
            name = parse_qual_name(p, name);
        if (p->tok.kind == P_LPAREN) {
            /* `name(...)` first-class callable: let postfix handle it by
             * producing A_CONSTREF; `name(args)` is a plain call */
            {
                Lex saved = p->lx;
                Token saved_tok = p->tok;
                advance(p);
                bool is_fcc = false;
                if (p->tok.kind == P_ELLIPSIS) {
                    advance(p);
                    is_fcc = (p->tok.kind == P_RPAREN);
                }
                p->lx = saved;
                p->tok = saved_tok;
                if (is_fcc) {
                    AExpr *e = ae_new(A_CONSTREF, line);
                    e->u.name = name;
                    return e;    /* postfix `(` sees A_CONSTREF + `(...)` */
                }
            }
            AExpr *e = ae_new(A_CALL, line);
            e->u.call.name = name;
            e->u.call.args = parse_arg_list(p);
            /* async(...)：调用形态 → 整体搬进协程；其余形态 → 统一走
             * 完全限定名 Tphp\async（免 use） */
            if (strcmp(tstr_val(name), "async") == 0
                || strcmp(tstr_val(name), "Tphp\\async") == 0) {
                if (e->u.call.args->n == 1
                    && ast_is_call_expr(e->u.call.args->items[0].e))
                    return make_async_call(e->u.call.args->items[0].e, line);
                e->u.call.name = tstr_intern("\\Tphp\\async", 11);
            }
            return e;
        }
        if (p->tok.kind == P_DOUBLE_COLON) {
            /* C::member static access (primary returns here bypass postfix) */
            return parse_static_access(p, name, line);
        }
        /* PHP magic constants — folded at parse time so each inlined require
         * keeps its own file identity (the AST has no per-node file field).
         * __FILE__/__DIR__ are the ORIGINAL source paths, packed or not; the
         * io layer transparently re-roots a disk-miss into the bundle. */
        if (strcmp(tstr_val(name), "__LINE__") == 0) {
            AExpr *e = ae_new(A_INT, line);
            e->u.i = line;
            return e;
        }
        if (strcmp(tstr_val(name), "__FILE__") == 0) {
            char ab[1024];
            parse_abspath(p->file, ab, sizeof ab);
            AExpr *e = ae_new(A_STR, line);
            e->u.name = tstr_intern(ab, strlen(ab));
            return e;
        }
        if (strcmp(tstr_val(name), "__DIR__") == 0) {
            char ab[1024];
            parse_abspath(p->file, ab, sizeof ab);
            size_t dlen = 0;
            const char *dir = parse_dirname(ab, &dlen);
            AExpr *e = ae_new(A_STR, line);
            e->u.name = tstr_intern(dir ? dir : ab, dir ? dlen : strlen(ab));
            return e;
        }
        /* PHP 8.4 __PROPERTY__：仅在属性钩子体内有定义；折为属性名字符串 */
        if (strcmp(tstr_val(name), "__PROPERTY__") == 0) {
            if (!g_hook_prop_name)
                p_error(p, "__PROPERTY__ can only be used inside a property hook");
            AExpr *e = ae_new(A_STR, line);
            e->u.name = g_hook_prop_name;
            return e;
        }
        AExpr *e = ae_new(A_CONSTREF, line);
        e->u.name = name;
        return e;
    }
    case KW_FUNCTION: {
        advance(p);
        return parse_closure(p, false);
    }
    case KW_FN: {
        advance(p);
        return parse_arrow_fn(p);
    }
    case KW_STATIC: {
        advance(p);
        if (p->tok.kind == KW_FUNCTION) {
            advance(p);
            return parse_closure(p, true);
        }
        if (p->tok.kind == KW_FN) {
            advance(p);
            return parse_arrow_fn(p);   /* static arrows: no $this anyway */
        }
        if (p->tok.kind == P_DOUBLE_COLON) {
            /* late static binding: static::m() / static::K */
            return parse_static_access(p, tstr_intern("static", 6), line);
        }
        p_error(p, "Expected 'function' or 'fn' after 'static'");
    }
    case KW_SELF:
    case KW_PARENT: {
        TStr *n = p->tok.kind == KW_SELF ? tstr_intern("self", 4)
                                         : tstr_intern("parent", 6);
        advance(p);
        if (p->tok.kind == P_DOUBLE_COLON)
            return parse_static_access(p, n, line);
        AExpr *e = ae_new(A_CONSTREF, line);
        e->u.name = n;
        return e;
    }
    case P_BACKSLASH: {
        /* fully qualified name: \Foo / \Foo\Bar (class, function, constant) */
        TStr *name = parse_qual_name(p, NULL);
        if (p->tok.kind == P_LPAREN) {
            AExpr *e = ae_new(A_CALL, line);
            e->u.call.name = name;
            e->u.call.args = parse_arg_list(p);
            /* async(...)：调用形态 → 整体搬进协程；其余形态 → 统一走
             * 完全限定名 Tphp\async（免 use） */
            if (strcmp(tstr_val(name), "async") == 0
                || strcmp(tstr_val(name), "Tphp\\async") == 0) {
                if (e->u.call.args->n == 1
                    && ast_is_call_expr(e->u.call.args->items[0].e))
                    return make_async_call(e->u.call.args->items[0].e, line);
                e->u.call.name = tstr_intern("\\Tphp\\async", 11);
            }
            return e;
        }
        if (p->tok.kind == P_DOUBLE_COLON)
            return parse_static_access(p, name, line);
        AExpr *e = ae_new(A_CONSTREF, line);
        e->u.name = name;
        return e;
    }
    case KW_NEW: {
        advance(p);
        /* anonymous class: new class(args) extends .. implements .. { body } */
        if (p->tok.kind == KW_CLASS) {
            advance(p);
            AArgList *args;
            bool paren_args = false;
            if (p->tok.kind == P_LPAREN) {
                args = parse_arg_list(p);
                paren_args = true;
            } else {
                args = tm_malloc(sizeof(AArgList));
                memset(args, 0, sizeof *args);   /* no-arg form */
            }
            AClassDecl *cd = tm_malloc(sizeof(AClassDecl));
            memset(cd, 0, sizeof *cd);
            cd->name = NULL;               /* anonymous */
            cd->kind = CK_CLASS;
            cd->line = line;
            if (accept(p, KW_EXTENDS)) cd->parent = parse_qual_name(p, NULL);
            if (accept(p, KW_IMPLEMENTS)) cd->ifaces = parse_name_list(p, &cd->n_ifaces);
            cd->body = parse_class_body(p, CK_CLASS, NULL);
            AExpr *e = ae_new(A_NEW, line);
            e->u.newx.name = NULL;
            e->u.newx.dyn = NULL;
            e->u.newx.args = args;
            e->u.newx.anon = cd;
            e->u.newx.paren_args = paren_args;
            return e;
        }
        TStr *name = NULL;
        AExpr *dyn = NULL;
        if (p->tok.kind == KW_SELF || p->tok.kind == KW_PARENT ||
            p->tok.kind == KW_STATIC) {
            /* new self / new parent / new static */
            name = p->tok.ident;   /* interned keyword text */
            advance(p);
        } else if (p->tok.kind == L_VAR) {
            /* dynamic class name: new $c / new $a->b / new $a['k']
             * (PHP new_variable: dim/prop postfixes, NO method calls —
             * `new $h->m()` is property fetch + ctor args) */
            dyn = parse_primary(p);
            for (;;) {
                if (p->tok.kind == P_LBRACKET) {
                    int ln = p->tok.line;
                    advance(p);
                    AExpr *idx = NULL;
                    if (p->tok.kind != P_RBRACKET) idx = parse_expr(p);
                    expect(p, P_RBRACKET, "Expected ']'");
                    AExpr *d = ae_new(A_DIM, ln);
                    d->u.dim.base = dyn;
                    d->u.dim.idx = idx;
                    dyn = d;
                } else if (p->tok.kind == P_OBJECT ||
                           p->tok.kind == P_NULLSAFE) {
                    bool ns = p->tok.kind == P_NULLSAFE;
                    int ln = p->tok.line;
                    advance(p);
                    if (p->tok.kind != L_IDENT && p->tok.kind != KW_NEW &&
                        p->tok.kind != KW_CLASS) {
                        p_error(p, "Expected property name after '->'");
                        break;
                    }
                    TStr *pname = p->tok.ident;
                    advance(p);
                    AExpr *pr = ae_new(A_PROP, ln);
                    pr->u.prop.obj = dyn;
                    pr->u.prop.name = pname;
                    pr->nullsafe = ns;
                    dyn = pr;
                } else break;
            }
        } else if (p->tok.kind == P_LPAREN) {
            /* new (expr) — parenthesized class-name expression */
            advance(p);
            dyn = parse_expr(p);
            expect(p, P_RPAREN, "Expected ')'");
        } else {
            name = parse_qual_name(p, NULL);
        }
        /* ctor args: optional (PHP new_non_dereferenceable has none) */
        AArgList *args;
        bool paren_args = false;
        if (p->tok.kind == P_LPAREN) {
            args = parse_arg_list(p);
            paren_args = true;
        } else {
            args = tm_malloc(sizeof(AArgList));
            memset(args, 0, sizeof *args);
        }
        AExpr *e = ae_new(A_NEW, line);
        e->u.newx.name = name;
        e->u.newx.dyn = dyn;
        e->u.newx.args = args;
        e->u.newx.anon = NULL;
        e->u.newx.paren_args = paren_args;
        return e;
    }
    case KW_CLONE: {
        /* clone <expr> 或 PHP 8.4 clone($obj, ['p'=>v]) */
        advance(p);
        AExpr *e = ae_new(A_CLONE, line);
        if (p->tok.kind == P_LPAREN) {
            advance(p);
            e->u.clone.e = parse_expr(p);
            e->u.clone.overrides = NULL;
            if (accept(p, P_COMMA))
                e->u.clone.overrides = parse_expr(p);   /* 覆盖数组 */
            expect(p, P_RPAREN, "Expected ')' after clone arguments");
        } else {
            e->u.clone.e = parse_unary(p);
            e->u.clone.overrides = NULL;
        }
        return e;
    }
    default:
        p_error(p, "Unexpected token in expression");
    }
}

/* ============ postfix ============ */
/* C::member static access shared by primary paths (L_IDENT, self, parent,
 * static) — these primaries return directly and bypass parse_postfix */
static AExpr *parse_static_access(Parser *p, TStr *name, int line) {
    advance(p);   /* consume '::' */
    TStr *member;
    if (p->tok.kind == KW_CLASS) {
        member = tstr_intern("class", 5);   /* C::class compile-time name */
        advance(p);
    } else if (p->tok.kind == L_VAR) {
        /* static property: the member name keeps a leading '$' so the
         * backend can tell `C::$prop` apart from `C::CONST`.
         * C1: `C::$$v`（静态属性动态名）/ `C::$v()`（静态方法动态名）→ aname */
        const char *v = tstr_val(p->tok.ident);
        size_t n = tstr_len(p->tok.ident);
        bool dyn_prop = v[0] == '$';
        TStr *vident = p->tok.ident;
        advance(p);
        bool dyn_call = p->tok.kind == P_LPAREN;
        if (dyn_prop || dyn_call) {
            AExpr *av = ae_new(A_VAR, line);
            av->u.name = dyn_prop ? tstr_intern(v + 1, n - 1) : vident;
            size_t mn0 = n - (dyn_prop ? 1 : 0);
            char *mb = tm_malloc(mn0 + 2);
            mb[0] = '$';
            memcpy(mb + 1, v + (dyn_prop ? 1 : 0), mn0);
            TStr *mem = tstr_intern(mb, mn0 + 1);
            tm_free(mb);
            if (dyn_call) {
                AExpr *sc = ae_new(A_STATICCALL, line);
                sc->u.scall.cls = name;
                sc->u.scall.member = mem;
                sc->u.scall.aname = av;
                sc->u.scall.args = parse_arg_list(p);
                return sc;
            }
            AExpr *sg = ae_new(A_STATICGET, line);
            sg->u.scall.cls = name;
            sg->u.scall.member = mem;
            sg->u.scall.aname = av;
            return sg;
        }
        char *tmp = tm_malloc(n + 1);
        tmp[0] = '$';
        memcpy(tmp + 1, v, n);        /* content only: buffer is n+1 total */
        member = tstr_intern(tmp, n + 1);
        tm_free(tmp);
        /* PHP 8.4 parent::$p::get() / parent::$p::set($v) */
        if (p->tok.kind == P_DOUBLE_COLON &&
            (name == tstr_intern("parent", 6) || strcmp(tstr_val(name), "parent") == 0)) {
            Lex saved_lx = p->lx;
            Token saved_tok = p->tok;
            advance(p);   /* consume '::' */
            if (p->tok.kind == L_IDENT &&
                (strcmp(tstr_val(p->tok.ident), "get") == 0 ||
                 strcmp(tstr_val(p->tok.ident), "set") == 0)) {
                bool is_set = tstr_val(p->tok.ident)[0] == 's';
                advance(p);
                AExpr *ph = ae_new(A_PROPHOOK, line);
                ph->u.phook.cls = name;
                ph->u.phook.prop = member;   /* 含 '$' 前缀 */
                ph->u.phook.is_set = is_set;
                ph->u.phook.args = parse_arg_list(p);
                return ph;
            }
            p->lx = saved_lx;
            p->tok = saved_tok;
        }
    } else if (p->tok.kind == L_IDENT) {
        member = p->tok.ident;
        advance(p);
    } else if (tok_is_member_name(p->tok.kind)) {
        /* keywords as member names: `Worker::self()`, `FFI::new()`,
         * `Status::from()` — PHP semi-reserved list */
        member = p->tok.ident;
        advance(p);
    } else {
        p_error(p, "Expected member name after '::'");
        member = NULL;
    }
    if (p->tok.kind == P_LPAREN) {
        AExpr *sc = ae_new(A_STATICCALL, line);
        sc->u.scall.cls = name;
        sc->u.scall.member = member;
        sc->u.scall.args = parse_arg_list(p);
        return sc;
    }
    AExpr *sg = ae_new(A_STATICGET, line);
    sg->u.scall.cls = name;
    sg->u.scall.member = member;
    return sg;
}

static AExpr *parse_postfix_from(Parser *p, AExpr *e) {
    for (;;) {
        int line = p->tok.line;
        switch (p->tok.kind) {
        case P_LBRACKET: {
            advance(p);
            AExpr *idx = NULL;
            if (p->tok.kind != P_RBRACKET) idx = parse_expr(p);
            expect(p, P_RBRACKET, "Expected ']'");
            AExpr *d = ae_new(A_DIM, line);
            d->u.dim.base = e;
            d->u.dim.idx = idx;
            e = d;
            break;
        }
        case P_LPAREN: {
            /* first-class callable: name(...) / $var(...) / $obj->m(...)
             * the parens contain a bare `...` */
            if (e->tag == A_CONSTREF || e->tag == A_VAR || e->tag == A_PROP) {
                Lex saved = p->lx;
                Token saved_tok = p->tok;
                advance(p);
                bool is_fcc = false;
                if (p->tok.kind == P_ELLIPSIS) {
                    advance(p);
                    is_fcc = (p->tok.kind == P_RPAREN);
                }
                if (is_fcc) {
                    advance(p);
                    if (e->tag == A_CONSTREF) {
                        AExpr *fc = ae_new(A_FIRSTCLASS, line);
                        fc->u.name = e->u.name;
                        return fc;
                    }
                    AExpr *fc = ae_new(A_FIRSTCLASS_DYN, line);
                    fc->u.e = e;
                    return fc;
                }
                p->lx = saved;
                p->tok = saved_tok;
            }
            /* P1-3: 链式动态调用 f()() / $obj->m()() / C::m()() —— 被调者
             * 可以是任意可调用表达式（运行期按值分发；此前一律 Parse
             * error）。非可调值由 op_init_dyn 报 "Value is not callable"。 */
            /* A paren-less `new X` is complete: a following `(` belongs to the
             * outer call (`get_class(new A)`), not to new — and it must not be
             * re-parsed as a method call. A paren-ful `new X(...)` already
             * consumed its args in KW_NEW, so this branch is unreachable for it. */
            if (e->tag == A_NEW && !e->u.newx.paren_args)
                return e;
            AArgList *args = parse_arg_list(p);
            AExpr *c = ae_new(A_CALL, line);
            c->u.call.dyn = e;
            c->u.call.args = args;
            e = c;
            break;
        }
        case P_OBJECT: {
            advance(p);
            if (p->tok.kind == L_VAR) {
                /* C1: $obj->$v / $obj->$v() —— 成员名 = 变量值（求值一次） */
                AExpr *av = ae_new(A_VAR, line);
                av->u.name = p->tok.ident;
                advance(p);
                if (p->tok.kind == P_LPAREN) {
                    Lex saved = p->lx;
                    Token saved_tok = p->tok;
                    advance(p);
                    bool is_fcc = false;
                    if (p->tok.kind == P_ELLIPSIS) {
                        advance(p);
                        is_fcc = (p->tok.kind == P_RPAREN);
                    }
                    if (is_fcc) {
                        advance(p);
                        AExpr *pr = ae_new(A_PROP, line);
                        pr->u.prop.obj = e;
                        pr->u.prop.aname = av;
                        AExpr *fc = ae_new(A_FIRSTCLASS_DYN, line);
                        fc->u.e = pr;
                        return fc;
                    }
                    p->lx = saved;
                    p->tok = saved_tok;
                    AArgList *args = parse_arg_list(p);
                    AExpr *m = ae_new(A_METHODCALL, line);
                    m->u.mcall.obj = e;
                    m->u.mcall.aname = av;
                    m->u.mcall.args = args;
                    e = m;
                } else {
                    AExpr *pr = ae_new(A_PROP, line);
                    pr->u.prop.obj = e;
                    pr->u.prop.aname = av;
                    e = pr;
                }
                break;
            }
            if (!tok_is_member_name(p->tok.kind)) {
                p_error(p, "Variable property access ($obj->$name) is not supported");
            }
            TStr *name = p->tok.ident;
            advance(p);
            if (p->tok.kind == P_LPAREN) {
                /* P2-A first-class callable: ->m(...) with a bare `...`
                 * produces a bound method closure expression */
                Lex saved = p->lx;
                Token saved_tok = p->tok;
                advance(p);
                bool is_fcc = false;
                if (p->tok.kind == P_ELLIPSIS) {
                    advance(p);
                    is_fcc = (p->tok.kind == P_RPAREN);
                }
                if (is_fcc) {
                    advance(p);
                    AExpr *pr = ae_new(A_PROP, line);
                    pr->u.prop.obj = e;
                    pr->u.prop.name = name;
                    AExpr *fc = ae_new(A_FIRSTCLASS_DYN, line);
                    fc->u.e = pr;
                    return fc;
                }
                p->lx = saved;
                p->tok = saved_tok;
                AArgList *args = parse_arg_list(p);
                /* $obj->spawn(EXPR)：与 async(EXPR) 同规则——单实参且是
                 * 调用表达式时包成 fn() => EXPR（spawn 是并发保留方法名） */
                if (args->n == 1 && ast_is_call_expr(args->items[0].e)
                    && strcmp(tstr_val(name), "spawn") == 0)
                    args->items[0].e = wrap_thunk(args->items[0].e, line);
                AExpr *m = ae_new(A_METHODCALL, line);
                m->u.mcall.obj = e;
                m->u.mcall.name = name;
                m->u.mcall.args = args;
                e = m;
            } else {
                AExpr *pr = ae_new(A_PROP, line);
                pr->u.prop.obj = e;
                pr->u.prop.name = name;
                e = pr;
            }
            break;
        }
        case P_INC:
        case P_DEC: {
            int op = p->tok.kind;
            advance(p);
            AExpr *u = ae_new(A_INCDEC, line);
            u->u.un.op = op;
            u->u.un.e = e;
            u->u.un.prefix = false;
            e = u;
            break;
        }
        case P_NULLSAFE: {
            /* P2-A: ?-> (nullsafe member access); the whole chain
             * short-circuits to null once any hop sees null — compile
             * layer emits per-hop JMP_NULL guards */
            advance(p);
            if (p->tok.kind == L_VAR) {
                /* C1: $obj?->$v */
                AExpr *av = ae_new(A_VAR, line);
                av->u.name = p->tok.ident;
                advance(p);
                if (p->tok.kind == P_LPAREN) {
                    AArgList *args = parse_arg_list(p);
                    AExpr *m = ae_new(A_METHODCALL, line);
                    m->u.mcall.obj = e;
                    m->u.mcall.aname = av;
                    m->u.mcall.args = args;
                    m->nullsafe = true;
                    e = m;
                } else {
                    AExpr *pr = ae_new(A_PROP, line);
                    pr->u.prop.obj = e;
                    pr->u.prop.aname = av;
                    pr->nullsafe = true;
                    e = pr;
                }
                break;
            }
            if (!tok_is_member_name(p->tok.kind)) {
                p_error(p, "Variable property access ($obj->$name) is not supported");
            }
            TStr *name = p->tok.ident;
            advance(p);
            if (p->tok.kind == P_LPAREN) {
                AArgList *args = parse_arg_list(p);
                if (args->n == 1 && ast_is_call_expr(args->items[0].e)
                    && strcmp(tstr_val(name), "spawn") == 0)
                    args->items[0].e = wrap_thunk(args->items[0].e, line);
                AExpr *m = ae_new(A_METHODCALL, line);
                m->u.mcall.obj = e;
                m->u.mcall.name = name;
                m->u.mcall.args = args;
                m->nullsafe = true;
                e = m;
            } else {
                AExpr *pr = ae_new(A_PROP, line);
                pr->u.prop.obj = e;
                pr->u.prop.name = name;
                pr->nullsafe = true;
                e = pr;
            }
            break;
        }
        case P_DOUBLE_COLON: {
            /* static access after a primary that bypassed postfix
             * (self::m()/parent::K/Static::m()/static::m()), the P2-B
             * $obj::$prop / $className::$prop, and the trailing `::class`
             * after any value (Status::Active::class) */
            TStr *cls = NULL;
            if (e->tag != A_CONSTREF) {
                advance(p);   /* 消费 '::'——此前 L_VAR 判断在消费前，永假 */
                if (p->tok.kind == L_VAR) {
                    /* P2-B: $obj::$staticProp（读/赋值目标）；
                     * C1: $obj::$$v → aname（动态静态属性名） */
                    const char *vn = tstr_val(p->tok.ident);
                    size_t vn_len = tstr_len(p->tok.ident);
                    bool dyn_prop = vn[0] == '$';
                    char sbuf[160];
                    sbuf[0] = '$';
                    size_t cl = vn_len < sizeof sbuf - 2 ? vn_len : sizeof sbuf - 2;
                    memcpy(sbuf + 1, vn, cl);
                    sbuf[cl + 1] = 0;
                    TStr *member = tstr_intern(sbuf, cl + 1);
                    AExpr *av = NULL;
                    if (dyn_prop) {
                        av = ae_new(A_VAR, line);
                        av->u.name = tstr_intern(vn + 1, vn_len - 1);
                    }
                    advance(p);
                    AExpr *os = ae_new(A_OBJSTATIC, line);
                    os->u.prop.obj = e;
                    os->u.prop.name = member;
                    os->u.prop.aname = av;
                    e = os;
                    break;
                }
                /* only `::class` may follow a value: Status::Active::class */
                if (p->tok.kind != KW_CLASS)
                    p_error(p, "Only '::class' may follow an expression");
                advance(p);
                AExpr *gc = ae_new(A_GETCLASS, line);
                gc->u.e = e;
                e = gc;
                break;
            }
            cls = e->u.name;
            advance(p);
            TStr *member;
            if (p->tok.kind == KW_CLASS) {
                member = tstr_intern("class", 5);
                advance(p);
            } else if (tok_is_member_name(p->tok.kind)) {
                member = p->tok.ident;   /* keywords usable as member names */
                advance(p);
            } else {
                p_error(p, "Expected member name after '::'");
                member = NULL;
            }
            if (p->tok.kind == P_LPAREN) {
                AExpr *sc = ae_new(A_STATICCALL, line);
                sc->u.scall.cls = cls;
                sc->u.scall.member = member;
                sc->u.scall.args = parse_arg_list(p);
                e = sc;
            } else {
                AExpr *sg = ae_new(A_STATICGET, line);
                sg->u.scall.cls = cls;
                sg->u.scall.member = member;
                e = sg;
            }
            break;
        }
        default:
            return e;
        }
    }
}

static AExpr *parse_postfix(Parser *p) {
    return parse_postfix_from(p, parse_primary(p));
}

/* ============ unary / pow ============ */

static const struct { const char *n; int c; } ctypes[] = {
    { "int", 1 }, { "integer", 1 }, { "bool", 2 }, { "boolean", 2 },
    { "float", 3 }, { "double", 3 }, { "real", 3 }, { "string", 4 },
    { "array", 5 }, { "void", 6 },   /* PHP 8.5 (void) cast：求值后丢弃 */
};

/* pow: right-assoc, exponent may be unary; valid after any primary-sized
 * operand including parenthesized groups and casts */
static AExpr *maybe_pow(Parser *p, AExpr *e) {
    if (p->tok.kind == P_POW) {
        int l2 = p->tok.line;
        advance(p);
        AExpr *r = parse_unary(p);
        AExpr *b = ae_new(A_BINARY, l2);
        b->u.bin.op = P_POW;
        b->u.bin.l = e;
        b->u.bin.r = r;
        return b;
    }
    return e;
}

static AExpr *parse_unary(Parser *p) {
    int line = p->tok.line;
    switch (p->tok.kind) {
    case P_PLUS:
    case P_MINUS:
    case P_TILDE: {
        int op = p->tok.kind;
        advance(p);
        AExpr *e = ae_new(A_UNARY, line);
        e->u.un.op = op;
        e->u.un.e = parse_unary(p);
        return e;
    }
    case P_NOT: {
        advance(p);
        AExpr *e = ae_new(A_UNARY, line);
        e->u.un.op = P_NOT;
        e->u.un.e = parse_unary(p);
        return e;
    }
    case P_INC:
    case P_DEC: {
        int op = p->tok.kind;
        advance(p);
        AExpr *e = ae_new(A_INCDEC, line);
        e->u.un.op = op;
        e->u.un.e = parse_unary(p);
        e->u.un.prefix = true;
        return e;
    }
    case P_LPAREN: {
        /* probe cast: '(' type ')' — backtrack when it is a plain group */
        Lex saved = p->lx;
        Token saved_tok = p->tok;
        advance(p);
        const char *cast_name = NULL;
        if (p->tok.kind == L_IDENT) {
            cast_name = tstr_val(p->tok.ident);
        } else if (p->tok.kind == KW_ARRAY) {
            cast_name = "array";
        }
        if (cast_name) {
            for (size_t i = 0; i < sizeof ctypes / sizeof ctypes[0]; i++) {
                if (strlen(cast_name) == strlen(ctypes[i].n) &&
                    strncasecmp(cast_name, ctypes[i].n, strlen(cast_name)) == 0) {
                    int ctype = ctypes[i].c;
                    advance(p);
                    expect(p, P_RPAREN, "Expected ')' after cast type");
                    AExpr *e = ae_new(A_CAST, line);
                    e->u.cast.ctype = ctype;
                    e->u.cast.e = parse_unary(p);
                    return e;
                }
            }
        }
        /* not a cast: restore and parse a group; postfix chains keep
         * applying after the group: ($obj)->m() */
        p->lx = saved;
        p->tok = saved_tok;
        advance(p);
        AExpr *e = parse_expr(p);
        expect(p, P_RPAREN, "Expected ')'");
        if (e->tag == A_TERNARY) e->u.tern.paren = true;  /* grouped */
        return maybe_pow(p, parse_postfix_from(p, e));
    }
    default:
        break;
    }
    return maybe_pow(p, parse_postfix(p));
}

/* ============ binary levels (lowest first) ============ */
static AExpr *bin_new(int op, AExpr *l, AExpr *r, int line) {
    AExpr *e = ae_new(A_BINARY, line);
    e->u.bin.op = op;
    e->u.bin.l = l;
    e->u.bin.r = r;
    return e;
}

static AExpr *parse_mul_level(Parser *p) {
    AExpr *e = parse_unary(p);
    while (p->tok.kind == P_MUL || p->tok.kind == P_DIV || p->tok.kind == P_MOD) {
        int op = p->tok.kind, line = p->tok.line;
        advance(p);
        e = bin_new(op, e, parse_unary(p), line);
    }
    return e;
}

static AExpr *parse_add_level(Parser *p) {
    AExpr *e = parse_mul_level(p);
    while (p->tok.kind == P_PLUS || p->tok.kind == P_MINUS) {
        int op = p->tok.kind, line = p->tok.line;
        advance(p);
        e = bin_new(op, e, parse_mul_level(p), line);
    }
    return e;
}

static AExpr *parse_shift_level(Parser *p) {
    AExpr *e = parse_add_level(p);
    while (p->tok.kind == P_SHL || p->tok.kind == P_SHR) {
        int op = p->tok.kind, line = p->tok.line;
        advance(p);
        e = bin_new(op, e, parse_add_level(p), line);
    }
    return e;
}

static bool is_arith_binop(AExpr *e) {
    if (e->tag != A_BINARY) return false;
    int op = e->u.bin.op;
    return op == P_PLUS || op == P_MINUS || op == P_MUL ||
           op == P_DIV || op == P_MOD;
}

static AExpr *parse_concat_level(Parser *p) {
    AExpr *e = parse_shift_level(p);
    while (p->tok.kind == P_DOT) {
        int line = p->tok.line;
        advance(p);
        AExpr *r = parse_shift_level(p);
        if (is_arith_binop(r))
            p_error(p, "Mixing '.' and arithmetic without parentheses is not allowed; add parentheses");
        e = bin_new(P_DOT, e, r, line);
    }
    return e;
}

static AExpr *parse_rel_level(Parser *p) {
    AExpr *e = parse_concat_level(p);
    for (;;) {
        if (p->tok.kind == P_LT || p->tok.kind == P_LE ||
            p->tok.kind == P_GT || p->tok.kind == P_GE) {
            int op = p->tok.kind, line = p->tok.line;
            advance(p);
            e = bin_new(op, e, parse_concat_level(p), line);
            continue;
        }
        if (p->tok.kind == KW_INSTANCEOF) {
            int line = p->tok.line;
            advance(p);
            AExpr *r = ae_new(A_CONSTREF, line);
            r->u.name = parse_qual_name(p, NULL);
            AExpr *io = ae_new(A_INSTANCEOF, line);
            io->u.bin.l = e;
            io->u.bin.r = r;
            e = io;
            continue;
        }
        break;
    }
    return e;
}

static AExpr *parse_eq_level(Parser *p) {
    AExpr *e = parse_rel_level(p);
    while (p->tok.kind == P_EQ || p->tok.kind == P_NE ||
           p->tok.kind == P_SEQ || p->tok.kind == P_SNE ||
           p->tok.kind == P_SPACESHIP) {
        int op = p->tok.kind, line = p->tok.line;
        advance(p);
        e = bin_new(op, e, parse_rel_level(p), line);
    }
    return e;
}

static AExpr *parse_bitand_level(Parser *p) {
    AExpr *e = parse_eq_level(p);
    while (p->tok.kind == P_AMP) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_AMP, e, parse_eq_level(p), line);
    }
    return e;
}

static AExpr *parse_bitxor_level(Parser *p) {
    AExpr *e = parse_bitand_level(p);
    while (p->tok.kind == P_CARET) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_CARET, e, parse_bitand_level(p), line);
    }
    return e;
}

static AExpr *parse_bitor_level(Parser *p) {
    AExpr *e = parse_bitxor_level(p);
    while (p->tok.kind == P_PIPE) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_PIPE, e, parse_bitxor_level(p), line);
    }
    return e;
}

static AExpr *parse_andand_level(Parser *p) {
    AExpr *e = parse_bitor_level(p);
    while (p->tok.kind == P_ANDAND) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_ANDAND, e, parse_bitor_level(p), line);
    }
    return e;
}

static AExpr *parse_oror_level(Parser *p) {
    AExpr *e = parse_andand_level(p);
    while (p->tok.kind == P_OROR) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_OROR, e, parse_andand_level(p), line);
    }
    return e;
}

static AExpr *parse_coalesce_level(Parser *p) {
    AExpr *e = parse_oror_level(p);
    if (p->tok.kind == P_COALESCE) {
        int line = p->tok.line;
        advance(p);
        AExpr *r = parse_coalesce_level(p); /* right-assoc */
        AExpr *c = ae_new(A_COALESCE, line);
        c->u.tern.c = e;
        c->u.tern.e = r;
        return c;
    }
    return e;
}

static AExpr *parse_ternary(Parser *p) {
    AExpr *e = parse_coalesce_level(p);
    int line = p->tok.line;
    if (p->tok.kind != P_QUESTION)
        return e;
    advance(p);
    AExpr *m = NULL;
    if (p->tok.kind == P_COLON) {
        advance(p);                       /* short ternary: cond ?: else */
    } else {
        m = parse_coalesce_level(p);
        expect(p, P_COLON, "Expected ':' in ternary");
    }
    AExpr *r = parse_ternary(p);
    if (r->tag == A_TERNARY && !r->u.tern.paren)
        p_error(p, "Nested ternaries must be parenthesized (PHP 8 rule)");
    AExpr *t = ae_new(A_TERNARY, line);
    t->u.tern.c = e;
    t->u.tern.m = m;
    t->u.tern.e = r;
    return t;
}

/* pipeline |> : left-assoc; sits between assignment and ternary */
static AExpr *parse_pipe_level(Parser *p) {
    AExpr *e = parse_ternary(p);
    while (p->tok.kind == P_PIPELINE) {
        int line = p->tok.line;
        advance(p);
        AExpr *rhs = parse_ternary(p);
        if (rhs->tag != A_CALL && rhs->tag != A_FIRSTCLASS)
            p_error(p, "Right side of |> must be a function call");
        AExpr *pl = ae_new(A_PIPELINE, line);
        pl->u.pipe.input = e;
        pl->u.pipe.call = rhs;
        e = pl;
    }
    return e;
}

static bool is_lvalue(AExpr *e) {
    if (!e) return false;
    if (e->tag == A_VAR || e->tag == A_DIM || e->tag == A_PROP) return true;
    /* self::$x / C::$x / $obj::$x（静态属性成员带 '$' 前缀）——P1-1：
     * 此前静态属性直接赋值全部报 "Cannot assign to this expression" */
    if (e->tag == A_STATICGET && tstr_val(e->u.scall.member)[0] == '$') return true;
    if (e->tag == A_OBJSTATIC) return true;   /* P2-B: $obj::$x */
    return false;
}

static AExpr *parse_assign_level(Parser *p) {
    /* PHP 8 generators：yield 优先级低于赋值（`$x = yield $v`）；
     * `yield k => v` 键值形态；yield from v2 后置。 */
    if (p->tok.kind == KW_YIELD) {
        int yline = p->tok.line;
        if (p->in_const_expr)
            p_error(p, "yield cannot be used in constant or default expressions");
        if (p->n_fn_stack == 0)
            p_error(p, "yield expression can only be used inside a function");
        p->fn_stack[p->n_fn_stack - 1]->is_generator = true;
        advance(p);
        if (p->tok.kind == KW_FROM)
            p_error(p, "'yield from' is not supported yet (planned v2; see PLAN/generators.md)");
        AExpr *first = parse_assign_level(p);
        AExpr *key = NULL, *val = first;
        if (accept(p, P_FATARROW)) {
            key = first;
            val = parse_assign_level(p);
        }
        AExpr *e = ae_new(A_YIELD, yline);
        e->u.yld.val = val;
        e->u.yld.key = key;
        return e;
    }
    AExpr *lhs = parse_pipe_level(p);
    int line = p->tok.line;
    int op = -1;
    switch (p->tok.kind) {
    case P_ASSIGN:
    case P_PLUS_EQ: case P_MINUS_EQ: case P_MUL_EQ: case P_DIV_EQ:
    case P_MOD_EQ: case P_POW_EQ: case P_DOT_EQ: case P_AND_EQ:
    case P_OR_EQ: case P_XOR_EQ: case P_SHL_EQ: case P_SHR_EQ:
    case P_COALESCE_EQ:
        op = p->tok.kind;
        break;
    default:
        return lhs;
    }
    advance(p);
    if (!is_lvalue(lhs))
        p_error(p, "Cannot assign to this expression");
    AExpr *rhs = parse_assign_level(p); /* right-assoc */
    if (op == P_ASSIGN) {
        AExpr *e = ae_new(A_ASSIGN, line);
        e->u.bin.op = P_ASSIGN;
        e->u.bin.l = lhs;
        e->u.bin.r = rhs;
        return e;
    }
    if (op == P_COALESCE_EQ) {
        AExpr *co = ae_new(A_COALESCE, line);
        co->u.tern.c = lhs;
        co->u.tern.e = rhs;
        AExpr *e = ae_new(A_ASSIGN, line);
        e->u.bin.op = P_ASSIGN;
        e->u.bin.l = lhs;
        e->u.bin.r = co;
        return e;
    }
    AExpr *e = ae_new(A_ASSIGN_OP, line);
    e->u.bin.op = op;
    e->u.bin.l = lhs;
    e->u.bin.r = rhs;
    return e;
}

/* keyword logical ops sit below assignment (PHP rule) */
static AExpr *parse_kw_and_level(Parser *p) {
    AExpr *e = parse_assign_level(p);
    while (p->tok.kind == KW_AND) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_ANDAND, e, parse_assign_level(p), line);
    }
    return e;
}

static AExpr *parse_kw_xor_level(Parser *p) {
    AExpr *e = parse_kw_and_level(p);
    while (p->tok.kind == KW_XOR) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_CARET, e, parse_kw_and_level(p), line);
    }
    return e;
}

static AExpr *parse_kw_or_level(Parser *p) {
    AExpr *e = parse_kw_xor_level(p);
    while (p->tok.kind == KW_OR) {
        int line = p->tok.line;
        advance(p);
        e = bin_new(P_OROR, e, parse_kw_xor_level(p), line);
    }
    return e;
}

static AExpr *parse_expr(Parser *p) { return parse_kw_or_level(p); }

/* 函数上下文栈（yield 标记目标） */
static void fn_push(Parser *p, struct AFunc *f) {
    if (p->n_fn_stack >= 64) p_error(p, "Function nesting too deep");
    p->fn_stack[p->n_fn_stack++] = f;
}
static void fn_pop(Parser *p) { p->n_fn_stack--; }

/* 常量/默认值表达式上下文：yield 禁止（PHP：constexpr 含 yield 报错） */
static AExpr *parse_expr_cctx(Parser *p) {
    int save = p->in_const_expr;
    p->in_const_expr = 1;
    AExpr *e = parse_expr(p);
    p->in_const_expr = save;
    return e;
}

/* ============ destructuring ============ */
static ADestructList *parse_destruct_list(Parser *p, bool bracket_form) {
    int line = p->tok.line;
    advance(p); /* '[' or list */
    if (!bracket_form) expect(p, P_LPAREN, "Expected '(' after list");
    ADestructList *dl = tm_malloc(sizeof(ADestructList));
    dl->items = NULL; dl->n = 0;
    dl->is_list_syntax = !bracket_form;
    LKind closer = bracket_form ? P_RBRACKET : P_RPAREN;
    while (p->tok.kind != closer) {
        ADestructItem *it;
        if (p->tok.kind == P_COMMA) {
            /* empty slot: skip */
            it = tm_malloc(sizeof(ADestructItem));
            memset(it, 0, sizeof *it);
        } else {
            it = tm_malloc(sizeof(ADestructItem));
            memset(it, 0, sizeof *it);
            if (p->tok.kind != P_COMMA) {
                AExpr *first = parse_expr(p);
                if (accept(p, P_FATARROW)) {
                    if (first->tag == A_INT) {
                        it->has_key = true;
                        it->int_key = first->u.i;
                    } else if (first->tag == A_STR) {
                        it->has_key = true;
                        it->str_key = first->u.name;
                    } else {
                        p_error(p, "Destructuring keys must be literal int/string in M1");
                    }
                    first = parse_expr(p);
                }
                it->target = first;
                if (first->tag != A_VAR)
                    p_error(p, "Destructuring target must be a variable (nested lists land in M2)");
            }
        }
        dl->items = tm_realloc(dl->items, sizeof(ADestructItem *) * (size_t)(dl->n + 1));
        dl->items[dl->n++] = it;
        if (!accept(p, P_COMMA)) break;
    }
    expect(p, closer, bracket_form ? "Expected ']'" : "Expected ')'");
    (void)line;
    return dl;
}

/* ============ function declarations ============ */
/* ============ type declarations (P1-1) ============
 * Parse a type declaration into a normalized interned string:
 *   "?int" / "int|string" / "?app\\model\\dog" / "static"
 * Builtin spellings are lowercased (PHP types are case-insensitive); class
 * names keep their declared spelling (matched case-insensitively later).
 * Returns NULL when no type is present. */
/* 交叉类型成员白名单（PHP 8.2：intersection 仅 class-like + object/self/parent/static；
 * 标量/数组/null 等 builtin 拼写拒绝）。seg 已经过小写化处理。 */
static bool type_seg_is_classlike(const char *seg) {
    static const char *bad[] = { "int", "float", "string", "bool", "array",
                                 "iterable", "mixed", "null", "true", "false",
                                 "callable", "closure", NULL };
    for (int i = 0; bad[i]; i++)
        if (strcmp(seg, bad[i]) == 0) return false;
    return true;   /* object/self/parent/static 与类名（含 \）都放行 */
}

/* 解析一个裸类型段（限定名或 builtin/伪类型关键字）到 buf（小写化 builtin）。
 * 无段可解析返回 false（不消费 token）。 */
static bool parse_type_one_seg(Parser *p, char *buf, size_t cap) {
    if (p->tok.kind == L_IDENT) {
        size_t k = 0;
        const char *s = tstr_val(p->tok.ident);
        for (; s[k] && k + 1 < cap; k++) buf[k] = s[k];
        buf[k] = 0;
        advance(p);
        while (p->tok.kind == P_BACKSLASH) {
            advance(p);
            if (p->tok.kind != L_IDENT) p_error(p, "Expected name after '\\'");
            size_t l = strlen(buf);
            if (l + 2 < cap) { buf[l] = '\\'; buf[l + 1] = 0; }
            const char *s2 = tstr_val(p->tok.ident);
            size_t m = strlen(buf), j = 0;
            for (; s2[j] && m + j + 1 < cap; j++) buf[m + j] = s2[j];
            buf[m + j] = 0;
            advance(p);
        }
        bool all_alpha = true;
        for (size_t j = 0; buf[j]; j++) {
            char ch = buf[j];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
                { all_alpha = false; break; }
        }
        if (all_alpha) {
            for (size_t j = 0; buf[j]; j++)
                if (buf[j] >= 'A' && buf[j] <= 'Z')
                    buf[j] = (char)(buf[j] + 32);
        }
        return true;
    }
    const char *kw = NULL;
    if (p->tok.kind == KW_ARRAY) kw = "array";
    else if (p->tok.kind == KW_SELF) kw = "self";
    else if (p->tok.kind == KW_PARENT) kw = "parent";
    else if (p->tok.kind == KW_STATIC) kw = "static";
    else if (p->tok.kind == KW_NULL) kw = "null";
    else if (p->tok.kind == KW_TRUE) kw = "true";
    else if (p->tok.kind == KW_FALSE) kw = "false";
    if (!kw) return false;
    snprintf(buf, cap, "%s", kw);
    advance(p);
    return true;
}

/* 类型声明：?T / T|U / T&U（交叉，PHP 8.2）/ (T&U)|V（DNF）。
 * 存储为字符串：交叉成员内 '&' 连接、union 用 '|'、括号不入库
 * （运行期 tphp_type_accepts 按 '|' split 后段内 '&' 即合取，语义自动正确）。 */
static TStr *parse_type_decl(Parser *p) {
    bool nullable = accept(p, P_QUESTION);
    char buf[512];
    size_t n = 0;
    int usegs = 0;          /* union 成员数 */
    bool has_amp = false;   /* 出现过交叉成员 */
    bool bare_amp = false;  /* 出现过未加括号的交叉成员 */
    for (;;) {
        bool paren = false;
        if (p->tok.kind == P_LPAREN) { paren = true; advance(p); }
        /* --- 一个 union 成员：裸段[&段]* 或 (裸段&段+) --- */
        int amps = 0;
        bool first_seg = true;
        for (;;) {
            char segbuf[256];
            if (!parse_type_one_seg(p, segbuf, sizeof segbuf)) {
                if (first_seg && usegs == 0 && !paren && !nullable) return NULL;
                if (first_seg && (usegs > 0 || paren || nullable))
                    p_error(p, "Expected type name");
                break;   /* & 后缺段 */
            }
            if (amps > 0 || (paren && !first_seg)) {
                if (!type_seg_is_classlike(segbuf))
                    p_error(p, "Intersection types support only class names, "
                               "object, self/static/parent");
            }
            size_t sl = strlen(segbuf);
            if (n + sl + 2 >= sizeof buf - 1)
                p_error(p, "Type declaration too long");
            if (first_seg && usegs > 0) buf[n++] = '|';
            if (!first_seg) buf[n++] = '&';
            memcpy(buf + n, segbuf, sl);
            n += sl;
            first_seg = false;
            if (!accept(p, P_AMP)) break;
            amps++;
        }
        if (paren) {
            if (amps == 0)
                p_error(p, "Parenthesized types must contain an intersection "
                           "(use (A&B), not (A))");
            expect(p, P_RPAREN, "Expected ')' after intersection type");
        }
        if (amps > 0) {
            has_amp = true;
            if (!paren) bare_amp = true;
        }
        usegs++;
        if (!accept(p, P_PIPE)) break;
    }
    if (bare_amp && usegs > 1)
        p_error(p, "Intersection types inside a union must be parenthesized "
                   "(write (A&B)|C)");
    if (nullable && has_amp)
        p_error(p, "'?' cannot modify an intersection type");
    if (!usegs) {
        if (nullable) p_error(p, "Expected type name after '?'");
        return NULL;
    }
    if (nullable) {
        if (n + 2 >= sizeof buf) p_error(p, "Type declaration too long");
        memmove(buf + 1, buf, n);
        buf[0] = '?';
        n += 1;
    }
    buf[n] = 0;
    return tstr_intern(buf, n);
}

/* PHP 8.4 asymmetric visibility: `private(set)` / `protected(set)` 后缀
 * （vis 关键字已消费，当前 token 为 '('）。 */
static bool accept_set_suffix(Parser *p) {
    if (p->tok.kind != P_LPAREN) return false;
    advance(p);
    if (!(p->tok.kind == L_IDENT && tstr_len(p->tok.ident) == 3 &&
          (tstr_val(p->tok.ident)[0] | 32) == 's' &&
          (tstr_val(p->tok.ident)[1] | 32) == 'e' &&
          (tstr_val(p->tok.ident)[2] | 32) == 't'))
        p_error(p, "Expected 'set' inside visibility(...)");
    advance(p);
    expect(p, P_RPAREN, "Expected ')' after set");
    return true;
}

/* shared parameter list (named functions, closures, arrows) */
static void parse_params(Parser *p, AFunc *f) {
    while (p->tok.kind != P_RPAREN) {
        /* constructor promotion: visibility prefix before the type/param */
        int pvis = -1;
        int pset = 0;
        bool pro_readonly = false;
        bool pro_final = false;
        for (;;) {
            if (p->tok.kind == KW_PUBLIC) {
                advance(p);
                if (p->tok.kind == P_LPAREN)
                    p_error(p, "Cannot use public(set)");
                pvis = CVIS_PUBLIC;
            } else if (p->tok.kind == KW_PROTECTED || p->tok.kind == KW_PRIVATE) {
                int v = p->tok.kind == KW_PRIVATE ? CVIS_PRIVATE : CVIS_PROTECTED;
                advance(p);
                if (accept_set_suffix(p)) pset = v;
                else pvis = v;
            }
            else if (p->tok.kind == KW_READONLY) { advance(p); pro_readonly = true; }
            else if (p->tok.kind == KW_FINAL) {
                /* PHP 8.5 final 提升属性：final 可在可见性前/后 */
                advance(p); pro_final = true;
            }
            else break;
        }
        TStr *ptype = parse_type_decl(p);
        bool variadic = accept(p, P_ELLIPSIS);
        if (p->tok.kind != L_VAR) p_error(p, "Expected parameter variable");
        AParam *par = tm_malloc(sizeof(AParam));
        memset(par, 0, sizeof *par);
        par->name = p->tok.ident;
        par->type = ptype;
        par->default_val = NULL;
        par->is_variadic = variadic;
        par->is_promoted = pvis >= 0 || pro_readonly || pset > 0 || pro_final;
        par->vis = pvis >= 0 ? pvis : CVIS_PUBLIC;
        par->set_vis = pset;
        par->is_readonly = pro_readonly;
        par->is_final = pro_final;
        if ((pvis >= 0 || pro_readonly) && variadic)
            p_error(p, "Cannot promote a variadic parameter");
        advance(p);
        if (accept(p, P_ASSIGN)) par->default_val = parse_expr_cctx(p);
        f->params = tm_realloc(f->params, sizeof(AParam *) * (size_t)(f->n_params + 1));
        f->params[f->n_params++] = par;
        if (!accept(p, P_COMMA)) break;
    }
}

/* ============ class declarations (M3) ============ */
static AClassBody *parse_class_body(Parser *p, int kind, TStr *cls_name);

/* ---- PHP 8.4 property hooks ----
 * 在属性名/默认值之后解析 `{ get ...; set ...; }`。
 * 当前 token 必须是 '{'。interface/abstract 允许仅声明（`get;`）。 */
static void parse_prop_hooks(Parser *p, AClassMember *m, int kind) {
    m->prop.has_hooks = true;
    expect(p, P_LBRACE, "Expected '{' for property hooks");
    while (p->tok.kind != P_RBRACE) {
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated property hook block");
        /* 短形式 `get => expr;` 的收尾分号；全形式 `{ }` 后通常无分号 */
        if (p->tok.kind == P_SEMI) { advance(p); continue; }
        bool hook_final = false;
        if (p->tok.kind == KW_FINAL) { advance(p); hook_final = true; }
        bool by_ref = false;
        if (p->tok.kind == P_AMP) { advance(p); by_ref = true; }
        if (p->tok.kind != L_IDENT)
            p_error(p, "Expected 'get' or 'set' in property hook block");
        const char *hn = tstr_val(p->tok.ident);
        bool is_get;
        if (strncasecmp(hn, "get", 3) == 0 && hn[3] == 0) is_get = true;
        else if (strncasecmp(hn, "set", 3) == 0 && hn[3] == 0) is_get = false;
        else p_error(p, "Expected 'get' or 'set' in property hook block");
        int hline = p->tok.line;
        advance(p);

        AFunc *fn = tm_malloc(sizeof(AFunc));
        memset(fn, 0, sizeof *fn);
        fn->line = hline;

        /* 可选参数表：set (type $value) / get() 空表 */
        if (p->tok.kind == P_LPAREN) {
            advance(p);
            if (p->tok.kind != P_RPAREN)
                parse_params(p, fn);
            expect(p, P_RPAREN, "Expected ')' after hook parameters");
        }

        if (p->tok.kind == P_SEMI) {
            /* 仅声明：interface / abstract 要求钩子存在但无实现 */
            advance(p);
            if (is_get) {
                if (m->prop.hook_get) p_error(p, "Duplicate get hook");
                m->prop.decl_get = true;
                if (by_ref) m->prop.get_by_ref = true;
            } else {
                if (m->prop.hook_set) p_error(p, "Duplicate set hook");
                m->prop.decl_set = true;
            }
            if (fn->params) tm_free(fn->params);
            tm_free(fn);
            continue;
        }

        if (p->tok.kind == P_FATARROW) {
            /* 短形式：get => expr / set => expr */
            advance(p);
            TStr *saved_hp = g_hook_prop_name;
            g_hook_prop_name = m->prop.name;   /* __PROPERTY__ 折叠上下文 */
            AExpr *e = parse_expr(p);
            g_hook_prop_name = saved_hp;
            AStmtList *body = asl_new();
            if (is_get) {
                AStmt *ret = as_new(S_RETURN, hline);
                ret->u.e = e;
                asl_push(body, ret);
            } else {
                /* set => expr：表达式结果写入 backing（钩子上下文内 $this->prop = 绕过 set） */
                AExpr *tv = ae_new(A_VAR, hline);
                tv->u.name = tstr_intern("this", 4);
                AExpr *pr = ae_new(A_PROP, hline);
                pr->u.prop.obj = tv;
                pr->u.prop.name = m->prop.name;
                AExpr *as = ae_new(A_ASSIGN, hline);
                as->u.bin.l = pr;
                as->u.bin.r = e;
                AStmt *st = as_new(S_EXPR, hline);
                st->u.e = as;
                asl_push(body, st);
            }
            fn->body = body;
        } else if (p->tok.kind == P_LBRACE) {
            advance(p);
            TStr *saved_hp = g_hook_prop_name;
            g_hook_prop_name = m->prop.name;   /* __PROPERTY__ 折叠上下文 */
            AStmtList *body = asl_new();
            while (p->tok.kind != P_RBRACE) {
                if (p->tok.kind == L_EOF) p_error(p, "Unterminated hook body");
                asl_push(body, parse_statement(p));
            }
            g_hook_prop_name = saved_hp;
            expect(p, P_RBRACE, "Expected '}' after hook body");
            fn->body = body;
        } else {
            p_error(p, "Expected '=>' or '{' for property hook body");
        }

        /* set 隐式参数 $value（未显式声明时补一个） */
        if (!is_get && fn->n_params == 0) {
            AParam *par = tm_malloc(sizeof(AParam));
            memset(par, 0, sizeof *par);
            par->name = tstr_intern("value", 5);
            fn->params = tm_malloc(sizeof(AParam *));
            fn->params[0] = par;
            fn->n_params = 1;
        }

        if (is_get) {
            if (m->prop.hook_get || m->prop.decl_get) p_error(p, "Duplicate get hook");
            m->prop.hook_get = fn;
            m->prop.get_final = hook_final;
            if (by_ref) m->prop.get_by_ref = true;
        } else {
            if (m->prop.hook_set || m->prop.decl_set) p_error(p, "Duplicate set hook");
            m->prop.hook_set = fn;
            m->prop.set_final = hook_final;
        }
    }
    expect(p, P_RBRACE, "Expected '}' for property hooks");
    if (!m->prop.hook_get && !m->prop.hook_set &&
        !m->prop.decl_get && !m->prop.decl_set)
        p_error(p, "Property hook block must contain at least one hook");
    (void)kind;
}

static TStr **parse_name_list(Parser *p, int *out_n) {
    TStr **names = NULL; int n = 0;
    for (;;) {
        names = tm_realloc(names, sizeof(TStr *) * (size_t)(n + 1));
        names[n++] = parse_qual_name(p, NULL);
        if (!accept(p, P_COMMA)) break;
    }
    *out_n = n;
    return names;
}

static AClassMember *parse_class_member(Parser *p, int kind, TStr *cls_name) {
    int line = p->tok.line;
    AClassMember *m = tm_malloc(sizeof(AClassMember));
    memset(m, 0, sizeof *m);
    m->line = line;
    m->vis = CVIS_PUBLIC;

    /* visibility / modifier prefix loop */
    for (;;) {
        if (p->tok.kind == KW_PUBLIC) {
            advance(p);
            if (p->tok.kind == P_LPAREN)
                p_error(p, "Cannot use public(set)");
            m->vis = CVIS_PUBLIC;
        } else if (p->tok.kind == KW_PROTECTED || p->tok.kind == KW_PRIVATE) {
            int v = p->tok.kind == KW_PRIVATE ? CVIS_PRIVATE : CVIS_PROTECTED;
            advance(p);
            if (accept_set_suffix(p)) m->set_vis = (uint8_t)v;
            else m->vis = (uint8_t)v;
        }
        else if (p->tok.kind == KW_STATIC) { advance(p); m->is_static = true; }
        else if (p->tok.kind == KW_ABSTRACT) { advance(p); m->is_abstract = true; }
        else if (p->tok.kind == KW_FINAL) { advance(p); m->is_final = true; }
        else if (p->tok.kind == KW_READONLY) { advance(p); m->is_readonly = true; }
        else break;
    }

    if (p->tok.kind == KW_FUNCTION) {
        advance(p);
        /* 方法名允许关键字（PHP 半保留字：function list()/fn()/match() 合法） */
        if (!tok_is_member_name(p->tok.kind)) p_error(p, "Expected method name");
        AFunc *f = tm_malloc(sizeof(AFunc));
        memset(f, 0, sizeof *f);
        f->name = p->tok.ident;
        f->line = line;
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after method name");
        parse_params(p, f);
        expect(p, P_RPAREN, "Expected ')'");
        if (accept(p, P_COLON)) f->ret_type = parse_type_decl(p);
        m->tag = CM_METHOD;
        m->func = f;
        if (kind == CK_INTERFACE && m->vis != CVIS_PUBLIC) {
            /* P3: PHP "Access type for interface method I::f() must be public" */
            char buf[256];
            snprintf(buf, sizeof buf,
                     "Access type for interface method %s::%s() must be public",
                     cls_name ? tstr_val(cls_name) : "?", tstr_val(f->name));
            p_error(p, buf);
        }
        if (p->tok.kind == P_LBRACE) {
            if (m->is_abstract) p_error(p, "Abstract method cannot have a body");
            if (kind == CK_INTERFACE)    /* P3: 接口方法不允许有方法体 */
                p_error(p, "Interface method must not have a body");
            advance(p);
            fn_push(p, f);
            AStmtList *l = asl_new();
            while (p->tok.kind != P_RBRACE) {
                if (p->tok.kind == L_EOF) p_error(p, "Unterminated method body");
                asl_push(l, parse_statement(p));
            }
            expect(p, P_RBRACE, "Expected '}'");
            fn_pop(p);
            f->body = l;
        } else {
            expect(p, P_SEMI, "Expected ';' or '{' after method signature");
            m->is_abstract = true;
        }
        return m;
    }

    if (p->tok.kind == KW_CONST) {
        advance(p);
        /* PHP 8.3 typed class constants：`const int X = 1;` / `const A|B X`。
         * 边界判定：类型段序列后跟"名字 ="；名字本身是 L_IDENT，靠
         * "L_IDENT + 前瞻 ="识别（恢复状态后由外层消费名字）。 */
        m->tag = CM_CONST;
        m->prop.type = NULL;
        bool maybe_type = (p->tok.kind == L_IDENT || p->tok.kind == KW_ARRAY ||
                           p->tok.kind == KW_STATIC || p->tok.kind == KW_SELF ||
                           p->tok.kind == KW_PARENT || p->tok.kind == P_QUESTION);
        if (maybe_type && p->tok.kind == L_IDENT) {
            /* L_IDENT：advance 后是 '=' → 它就是名字（无类型）；否则是类型段 */
            Lex save_lx = p->lx;
            Token save_tok = p->tok;
            advance(p);
            bool is_name = (p->tok.kind == P_ASSIGN);
            p->lx = save_lx;
            p->tok = save_tok;
            maybe_type = !is_name;
        }
        if (maybe_type) {
            char tbuf[512];
            size_t tn = 0;
            int tsegs = 0, tamps_total = 0;
            for (;;) {
                /* 前瞻：当前 L_IDENT 后是 '=' → 到名字了，结束类型 */
                if (p->tok.kind == L_IDENT) {
                    Lex sp = p->lx;
                    Token st = p->tok;
                    advance(p);
                    bool at_name = (p->tok.kind == P_ASSIGN);
                    p->lx = sp; p->tok = st;
                    if (at_name) break;
                }
                char seg[256];
                if (!parse_type_one_seg(p, seg, sizeof seg))
                    p_error(p, "Expected type or constant name after 'const'");
                size_t sl2 = strlen(seg);
                if (tn + sl2 + 2 >= sizeof tbuf)
                    p_error(p, "Type declaration too long");
                if (tsegs) tbuf[tn++] = '|';
                memcpy(tbuf + tn, seg, sl2);
                tn += sl2;
                tsegs++;
                if (accept(p, P_AMP)) {
                    /* 交叉段（class-like 白名单） */
                    if (!type_seg_is_classlike(seg))
                        p_error(p, "Intersection types support only class names, "
                                   "object, self/static/parent");
                    tamps_total++;
                    /* 第二段起：& 拼进当前成员（不加 |） */
                    for (;;) {
                        if (p->tok.kind == L_IDENT) {
                            Lex sp = p->lx;
                            Token st = p->tok;
                            advance(p);
                            bool at_name = (p->tok.kind == P_ASSIGN);
                            p->lx = sp; p->tok = st;
                            if (at_name)
                                p_error(p, "Expected type after '&'");
                        }
                        char seg2[256];
                        if (!parse_type_one_seg(p, seg2, sizeof seg2))
                            p_error(p, "Expected type after '&'");
                        if (!type_seg_is_classlike(seg2))
                            p_error(p, "Intersection types support only class "
                                       "names, object, self/static/parent");
                        size_t l2 = strlen(seg2);
                        if (tn + l2 + 2 >= sizeof tbuf)
                            p_error(p, "Type declaration too long");
                        tbuf[tn++] = '&';
                        memcpy(tbuf + tn, seg2, l2);
                        tn += l2;
                        tamps_total++;
                        if (!accept(p, P_AMP)) break;
                    }
                    break;   /* 交叉成员后 v1 不支持再接 union（如需 DNF 用括号完整形态后置） */
                }
                if (!accept(p, P_PIPE)) break;
            }
            (void)tamps_total;
            tbuf[tn] = 0;
            m->prop.type = tstr_intern(tbuf, tn);
        }
        if (p->tok.kind != L_IDENT) p_error(p, "Expected constant name");
        m->prop.name = p->tok.ident;
        advance(p);
        expect(p, P_ASSIGN, "Expected '=' in class constant");
        m->prop.def = parse_expr_cctx(p);
        expect(p, P_SEMI, "Expected ';'");
        return m;
    }

    if (p->tok.kind == KW_USE) {
        advance(p);
        m->tag = CM_USE;
        m->use.names = parse_name_list(p, &m->use.n);
        AModList *mods = tm_malloc(sizeof(AModList));
        mods->items = NULL; mods->n = 0;
        if (p->tok.kind == P_LBRACE) {
            advance(p);
            while (p->tok.kind != P_RBRACE) {
                AMod mod;
                memset(&mod, 0, sizeof mod);
                mod.new_vis = -1;
                mod.trait = NULL;
                if (p->tok.kind == L_IDENT || p->tok.kind == P_BACKSLASH)
                    mod.trait = parse_qual_name(p, NULL);   /* qualified trait name */
                if (p->tok.kind == P_DOUBLE_COLON) {
                    advance(p);
                    if (!tok_is_member_name(p->tok.kind))
                        p_error(p, "Expected method name after '::'");
                    mod.method = p->tok.ident;
                    advance(p);
                } else {
                    mod.method = mod.trait;
                    mod.trait = NULL;
                }
                if (accept(p, KW_INSTEADOF)) {
                    mod.is_insteadof = true;
                    mod.from = parse_name_list(p, &mod.n_from);
                } else if (accept(p, KW_AS)) {
                    if (p->tok.kind == KW_PUBLIC) { advance(p); mod.new_vis = CVIS_PUBLIC; }
                    else if (p->tok.kind == KW_PROTECTED) { advance(p); mod.new_vis = CVIS_PROTECTED; }
                    else if (p->tok.kind == KW_PRIVATE) { advance(p); mod.new_vis = CVIS_PRIVATE; }
                    if (p->tok.kind == L_IDENT) {
                        mod.new_name = p->tok.ident;
                        advance(p);
                    }
                } else {
                    p_error(p, "Expected 'as' or 'insteadof' in trait adaptation");
                }
                expect(p, P_SEMI, "Expected ';'");
                mods->items = tm_realloc(mods->items, sizeof(AMod) * (size_t)(mods->n + 1));
                mods->items[mods->n++] = mod;
            }
            expect(p, P_RBRACE, "Expected '}'");
        } else {
            expect(p, P_SEMI, "Expected ';' after use");
        }
        m->use.mods = mods;
        return m;
    }

    /* property: [type] $var [= default] — P1-1 typed properties
     * PHP 8.4: 属性名后可接 `{ get/set }` 钩子块（替代 ';'） */
    if (p->tok.kind == L_VAR ||
        p->tok.kind == P_QUESTION || p->tok.kind == L_IDENT ||
        p->tok.kind == KW_ARRAY || p->tok.kind == KW_SELF ||
        p->tok.kind == KW_PARENT || p->tok.kind == KW_STATIC ||
        p->tok.kind == KW_NULL || p->tok.kind == KW_TRUE ||
        p->tok.kind == KW_FALSE) {
        TStr *ptype = parse_type_decl(p);
        if (p->tok.kind != L_VAR) p_error(p, "Expected property variable after type");
        if (kind == CK_INTERFACE && p->tok.kind == L_VAR) {
            /* 接口仅允许带钩子声明的属性（public string $x { get; }）；
             * 无钩子的普通属性仍然禁止 —— 但 `{` 还没看到，先放行到钩子判定 */
        }
        m->tag = CM_PROP;
        m->prop.name = p->tok.ident;
        m->prop.type = ptype;
        m->prop.def = NULL;
        m->next = NULL;
        advance(p);
        if (accept(p, P_ASSIGN)) m->prop.def = parse_expr_cctx(p);

        /* 钩子块：`{ get ...; set ...; }` —— 替代 ';'（php-src hooked_property
         * 文法：T_VARIABLE '{' hook_list '}' 后无分号），且禁止逗号链 */
        if (p->tok.kind == P_LBRACE) {
            if (kind == CK_INTERFACE && m->vis != CVIS_PUBLIC)
                p_error(p, "Access type for interface property must be public");
            if (m->is_static)
                p_error(p, "Property hooks are not supported on static properties");
            if (m->is_readonly)
                p_error(p, "Cannot declare readonly property with hooks");
            parse_prop_hooks(p, m, kind);
            return m;
        }

        if (kind == CK_INTERFACE)
            p_error(p, "Interfaces may not include properties");

        /* PHP: `public $a, $b;` / `public $a = 1, $b = 2;` — chain the extras
         * off `next`; the class-body walker flattens them into the item list. */
        AClassMember *tail = m;
        while (p->tok.kind == P_COMMA) {
            advance(p);
            if (p->tok.kind != L_VAR) { p_error(p, "Expected property variable after ','"); break; }
            AClassMember *nx = tm_malloc(sizeof(AClassMember));
            memset(nx, 0, sizeof *nx);
            nx->tag = CM_PROP;
            nx->line = line;
            nx->vis = m->vis;
            nx->set_vis = m->set_vis;
            nx->is_static = m->is_static;
            nx->is_readonly = m->is_readonly;
            nx->prop.name = p->tok.ident;
            nx->prop.type = ptype;
            nx->prop.def = NULL;
            advance(p);
            if (accept(p, P_ASSIGN)) nx->prop.def = parse_expr_cctx(p);
            tail->next = nx;
            tail = nx;
        }
        expect(p, P_SEMI, "Expected ';'");
        return m;
    }

    p_error(p, "Unexpected token in class body");
    return NULL;
}

static AClassBody *parse_class_body(Parser *p, int kind, TStr *cls_name) {
    expect(p, P_LBRACE, "Expected '{' for class body");
    AClassBody *b = tm_malloc(sizeof(AClassBody));
    b->items = NULL; b->n = 0;
    while (p->tok.kind != P_RBRACE) {
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated class body");
        if (kind == CK_ENUM && p->tok.kind == KW_CASE) {
            /* enum case: `case Name;` or `case Name = value;` */
            int line = p->tok.line;
            advance(p);
            if (p->tok.kind != L_IDENT) p_error(p, "Expected enum case name");
            AClassMember *m = tm_malloc(sizeof(AClassMember));
            memset(m, 0, sizeof *m);
            m->line = line;
            m->tag = CM_CASE;
            m->vis = CVIS_PUBLIC;
            m->prop.name = p->tok.ident;
            advance(p);
            if (accept(p, P_ASSIGN)) m->prop.def = parse_expr_cctx(p);
            expect(p, P_SEMI, "Expected ';'");
            b->items = tm_realloc(b->items, sizeof(AClassMember *) * (size_t)(b->n + 1));
            b->items[b->n++] = m;
            continue;
        }
        AClassMember *m = parse_class_member(p, kind, cls_name);
        /* flatten CM_PROP comma chains (`public $a, $b;`) into separate items */
        while (m) {
            AClassMember *nx = m->next;
            m->next = NULL;
            b->items = tm_realloc(b->items, sizeof(AClassMember *) * (size_t)(b->n + 1));
            b->items[b->n++] = m;
            m = nx;
        }
    }
    expect(p, P_RBRACE, "Expected '}'");
    return b;
}

/* class/interface/trait/enum declaration; leading flags already consumed */
static AStmt *parse_class_decl(Parser *p, uint8_t flags, int kind, int line) {
    if (p->tok.kind != L_IDENT) p_error(p, "Expected class name");
    AClassDecl *cd = tm_malloc(sizeof(AClassDecl));
    memset(cd, 0, sizeof *cd);
    cd->name = p->tok.ident;
    cd->kind = kind;
    cd->flags = flags;
    cd->line = line;
    advance(p);
    if (kind == CK_CLASS || kind == CK_INTERFACE) {
        if (accept(p, KW_EXTENDS)) {
            if (kind == CK_INTERFACE)
                /* an interface may extend several interfaces at once */
                cd->ifaces = parse_name_list(p, &cd->n_ifaces);
            else
                cd->parent = parse_qual_name(p, NULL);
        }
    }
    if (kind == CK_CLASS && accept(p, KW_IMPLEMENTS)) {
        cd->ifaces = parse_name_list(p, &cd->n_ifaces);
    }
    if (kind == CK_ENUM) {
        if (accept(p, P_COLON)) {
            if (p->tok.kind == L_IDENT) {
                cd->ifaces = NULL; cd->n_ifaces = 0;
                /* backing type carried via ifaces[0] marker later; store here */
                cd->backing_marker = p->tok.ident;
                advance(p);
            } else p_error(p, "Expected enum backing type");
        }
        if (accept(p, KW_IMPLEMENTS))
            cd->ifaces = parse_name_list(p, &cd->n_ifaces);
    }
    cd->body = parse_class_body(p, kind, cd->name);
    AStmt *s = as_new(S_CLASSDECL, line);
    s->u.cls = cd;
    return s;
}

/* closure literal: function (params) use ($a, $b) { body } — 'function'
 * already consumed */
static AExpr *parse_closure(Parser *p, bool is_static) {
    int line = p->tok.line;
    AFunc *f = tm_malloc(sizeof(AFunc));
    memset(f, 0, sizeof *f);
    f->is_static = is_static;
    f->line = line;
    expect(p, P_LPAREN, "Expected '('");
    parse_params(p, f);
    expect(p, P_RPAREN, "Expected ')'");
    if (p->tok.kind == KW_USE) {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after use");
        f->uses = ael_new();
        while (p->tok.kind != P_RPAREN) {
            if (accept(p, P_AMP))
                p_error(p, "By-reference closure captures are not supported (capture by value)");
            if (p->tok.kind != L_VAR) p_error(p, "Expected variable in use()");
            AExpr *v = ae_new(A_VAR, p->tok.line);
            v->u.name = p->tok.ident;
            ael_push(f->uses, v);
            advance(p);
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_RPAREN, "Expected ')'");
    }
    if (accept(p, P_COLON)) f->ret_type = parse_type_decl(p);
    expect(p, P_LBRACE, "Expected '{' for closure body");
    fn_push(p, f);
    AStmtList *l = asl_new();
    while (p->tok.kind != P_RBRACE) {
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated closure body");
        asl_push(l, parse_statement(p));
    }
    expect(p, P_RBRACE, "Expected '}'");
    fn_pop(p);
    f->body = l;
    AExpr *e = ae_new(A_CLOSURE, line);
    e->u.clo.func = f;
    e->u.clo.is_static = is_static;
    return e;
}

/* arrow function: fn(params) => expr — auto capture by value */
static AExpr *parse_arrow_fn(Parser *p) {
    int line = p->tok.line;
    AFunc *f = tm_malloc(sizeof(AFunc));
    memset(f, 0, sizeof *f);
    f->line = line;
    expect(p, P_LPAREN, "Expected '(' after fn");
    parse_params(p, f);
    expect(p, P_RPAREN, "Expected ')'");
    if (accept(p, P_COLON)) f->ret_type = parse_type_decl(p);
    expect(p, P_FATARROW, "Expected '=>' after arrow function parameters");
    fn_push(p, f);
    AStmtList *l = asl_new();
    AStmt *ret = as_new(S_RETURN, line);
    ret->u.e = parse_expr(p);
    asl_push(l, ret);
    fn_pop(p);
    f->body = l;
    AExpr *e = ae_new(A_ARROW, line);
    e->u.clo.func = f;
    return e;
}

static AStmt *parse_function_decl(Parser *p) {
    int line = p->tok.line; /* 'function' already consumed */
    TStr *name = expect_ident(p, "Expected function name");
    expect(p, P_LPAREN, "Expected '(' after function name");
    AFunc *f = tm_malloc(sizeof(AFunc));
    memset(f, 0, sizeof *f);
    f->name = name;
    f->line = line;
    parse_params(p, f);
    expect(p, P_RPAREN, "Expected ')'");
    if (accept(p, P_COLON)) f->ret_type = parse_type_decl(p); /* P1-1: 运行期校验 */
    expect(p, P_LBRACE, "Expected '{' for function body");
    fn_push(p, f);
    AStmtList *l = asl_new();
    while (p->tok.kind != P_RBRACE) {
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated function body");
        asl_push(l, parse_statement(p));
    }
    expect(p, P_RBRACE, "Expected '}'");
    fn_pop(p);
    f->body = l;
    AStmt *s = as_new(S_FUNCDECL, line);
    s->u.func = f;
    return s;
}

/* ============ statements ============ */
static AStmtList *parse_block(Parser *p) {
    expect(p, P_LBRACE, "Expected '{'");
    AStmtList *l = asl_new();
    while (p->tok.kind != P_RBRACE) {
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated block");
        asl_push(l, parse_statement(p));
    }
    expect(p, P_RBRACE, "Expected '}'");
    return l;
}

/* statements inside switch body (stop at case/default) */
static AStmtList *parse_switch_body(Parser *p) {
    AStmtList *l = asl_new();
    for (;;) {
        if (p->tok.kind == KW_CASE || p->tok.kind == KW_DEFAULT || p->tok.kind == P_RBRACE) break;
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated switch");
        asl_push(l, parse_statement(p));
    }
    return l;
}

static AStmt *parse_statement(Parser *p) {
    int line = p->tok.line;
    switch (p->tok.kind) {
    case P_SEMI:
        advance(p);
        return as_new(S_NOP, line);
    case P_LBRACE: {
        AStmt *s = as_new(S_BLOCK, line);
        s->u.stmts = parse_block(p);
        return s;
    }
    case L_INLINE_HTML: {
        TStr *html = p->tok.ident;
        advance(p);
        AExprList *args = ael_new();
        AExpr *e = ae_new(A_STR, line);
        e->u.name = html;
        ael_push(args, e);
        AStmt *s = as_new(S_ECHO, line);
        s->u.exprs = args;
        return s;
    }
    case KW_ECHO: {
        advance(p);
        AExprList *args = ael_new();
        for (;;) {
            ael_push(args, parse_expr(p));
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_SEMI, "Expected ';' after echo");
        AStmt *s = as_new(S_ECHO, line);
        s->u.exprs = args;
        return s;
    }
    case KW_IF: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after if");
        AExpr *cond = parse_expr(p);
        expect(p, P_RPAREN, "Expected ')'");
        AStmt *s = as_new(S_IF, line);
        AIfChain *chain = tm_malloc(sizeof(AIfChain));
        chain->branches = tm_malloc(sizeof(AIfBranch));
        chain->branches[0].cond = cond;
        chain->branches[0].body = (p->tok.kind == P_LBRACE)
                                      ? parse_block(p)
                                      : asl_wrap(parse_statement(p));
        chain->n = 1;
        chain->else_body = NULL;
        while (p->tok.kind == KW_ELSEIF) {
            advance(p);
            expect(p, P_LPAREN, "Expected '(' after elseif");
            AExpr *c2 = parse_expr(p);
            expect(p, P_RPAREN, "Expected ')'");
            chain->branches = tm_realloc(chain->branches,
                                         sizeof(AIfBranch) * (size_t)(chain->n + 1));
            chain->branches[chain->n].cond = c2;
            chain->branches[chain->n].body = (p->tok.kind == P_LBRACE)
                                                 ? parse_block(p)
                                                 : asl_wrap(parse_statement(p));
            chain->n++;
        }
        if (p->tok.kind == KW_ELSE) {
            advance(p);
            chain->else_body = (p->tok.kind == P_LBRACE)
                                   ? parse_block(p)
                                   : asl_wrap(parse_statement(p));
        }
        s->u.ifs = chain;
        return s;
    }
    case KW_WHILE: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after while");
        AExpr *cond = parse_expr(p);
        expect(p, P_RPAREN, "Expected ')'");
        AStmt *s = as_new(S_WHILE, line);
        s->u.wh.cond = cond;
        s->u.wh.body = (p->tok.kind == P_LBRACE) ? parse_block(p)
                                                 : asl_wrap(parse_statement(p));
        return s;
    }
    case KW_DO: {
        advance(p);
        AStmtList *body = (p->tok.kind == P_LBRACE) ? parse_block(p)
                                                    : asl_wrap(parse_statement(p));
        expect(p, KW_WHILE, "Expected 'while' after do block");
        expect(p, P_LPAREN, "Expected '('");
        AExpr *cond = parse_expr(p);
        expect(p, P_RPAREN, "Expected ')'");
        expect(p, P_SEMI, "Expected ';'");
        AStmt *s = as_new(S_DOWHILE, line);
        s->u.wh.cond = cond;
        s->u.wh.body = body;
        return s;
    }
    case KW_FOR: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after for");
        AFor *f = tm_malloc(sizeof(AFor));
        memset(f, 0, sizeof *f);
        f->init = ael_new();
        f->step = ael_new();
        if (p->tok.kind != P_SEMI) {
            for (;;) {
                ael_push(f->init, parse_expr(p));
                if (!accept(p, P_COMMA)) break;
            }
        }
        expect(p, P_SEMI, "Expected ';'");
        if (p->tok.kind != P_SEMI) f->cond = parse_expr(p);
        expect(p, P_SEMI, "Expected ';'");
        if (p->tok.kind != P_RPAREN) {
            for (;;) {
                ael_push(f->step, parse_expr(p));
                if (!accept(p, P_COMMA)) break;
            }
        }
        expect(p, P_RPAREN, "Expected ')'");
        f->body = (p->tok.kind == P_LBRACE) ? parse_block(p)
                                            : asl_wrap(parse_statement(p));
        AStmt *s = as_new(S_FOR, line);
        s->u.fors = f;
        return s;
    }
    case KW_FOREACH: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after foreach");
        AExpr *arr = parse_expr(p);
        expect(p, KW_AS, "Expected 'as' in foreach");
        AForeach *fe = tm_malloc(sizeof(AForeach));
        memset(fe, 0, sizeof *fe);
        fe->arr = arr;
        AExpr *first = parse_expr(p);
        if (first->tag != A_VAR)
            p_error(p, "foreach target must be a variable (references not supported)");
        if (accept(p, P_FATARROW)) {
            fe->has_key = true;
            fe->key_name = first->u.name;
            AExpr *second = parse_expr(p);
            if (second->tag != A_VAR)
                p_error(p, "foreach value target must be a variable");
            fe->val_name = second->u.name;
        } else {
            fe->val_name = first->u.name;
        }
        expect(p, P_RPAREN, "Expected ')'");
        fe->body = (p->tok.kind == P_LBRACE) ? parse_block(p)
                                             : asl_wrap(parse_statement(p));
        AStmt *s = as_new(S_FOREACH, line);
        s->u.fe = fe;
        return s;
    }
    case KW_SWITCH: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after switch");
        AExpr *cond = parse_expr(p);
        expect(p, P_RPAREN, "Expected ')'");
        expect(p, P_LBRACE, "Expected '{'");
        ASwitch *sw = tm_malloc(sizeof(ASwitch));
        memset(sw, 0, sizeof *sw);
        sw->cond = cond;
        int cap = 0;
        while (p->tok.kind != P_RBRACE) {
            if (sw->n == cap) {
                cap = cap ? cap * 2 : 8;
                sw->cases = tm_realloc(sw->cases, sizeof(ACase) * (size_t)cap);
            }
            ACase *cs = &sw->cases[sw->n++];
            memset(cs, 0, sizeof *cs);
            if (accept(p, KW_DEFAULT)) {
                cs->is_default = true;
            } else {
                expect(p, KW_CASE, "Expected 'case' or 'default'");
                cs->cond = parse_expr(p);
            }
            expect(p, P_COLON, "Expected ':' after case");
            cs->body = parse_switch_body(p);
        }
        expect(p, P_RBRACE, "Expected '}'");
        AStmt *s = as_new(S_SWITCH, line);
        s->u.sw = sw;
        return s;
    }
    case KW_BREAK:
    case KW_CONTINUE: {
        int tag = p->tok.kind == KW_BREAK ? S_BREAK : S_CONTINUE;
        advance(p);
        int64_t level = 1;
        if (p->tok.kind == L_INT) {
            if (p->tok.i < 1) p_error(p, "break/continue level must be >= 1");
            level = p->tok.i;
            advance(p);
        }
        expect(p, P_SEMI, "Expected ';'");
        AStmt *s = as_new(tag, line);
        s->u.level = level;
        return s;
    }
    case KW_RETURN: {
        advance(p);
        AStmt *s = as_new(S_RETURN, line);
        if (p->tok.kind != P_SEMI) s->u.e = parse_expr(p);
        expect(p, P_SEMI, "Expected ';'");
        return s;
    }
    case KW_GLOBAL: {
        advance(p);
        AStmt *s = as_new(S_GLOBAL, line);
        TStr **names = NULL;
        int n = 0;
        for (;;) {
            if (p->tok.kind != L_VAR) p_error(p, "Expected variable after global");
            names = tm_realloc(names, sizeof(TStr *) * (size_t)(n + 1));
            names[n++] = p->tok.ident;
            advance(p);
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_SEMI, "Expected ';'");
        s->u.names.names = names;
        s->u.names.n = n;
        return s;
    }
    case KW_UNSET: {
        advance(p);
        expect(p, P_LPAREN, "Expected '(' after unset");
        AExprList *args = ael_new();
        while (p->tok.kind != P_RPAREN) {
            ael_push(args, parse_expr(p));
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_RPAREN, "Expected ')'");
        expect(p, P_SEMI, "Expected ';'");
        AStmt *s = as_new(S_UNSET, line);
        s->u.exprs = args;
        return s;
    }
    case KW_CONST: {
        advance(p);
        AStmt *s = as_new(S_CONSTDECL, line);
        AConstDecl *decls = NULL;
        int n = 0;
        for (;;) {
            TStr *name = expect_ident(p, "Expected constant name");
            expect(p, P_ASSIGN, "Expected '=' after constant name");
            AExpr *value = parse_expr(p);
            decls = tm_realloc(decls, sizeof(AConstDecl) * (size_t)(n + 1));
            decls[n].name = name;
            decls[n].value = value;
            n++;
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_SEMI, "Expected ';'");
        s->u.consts.decls = decls;
        s->u.consts.n = n;
        return s;
    }
    case KW_FUNCTION: {
        /* named declarations inside namespace blocks are still hoisted:
         * pass_sigs/pass_funcs unfold S_BLOCK during compilation */
        advance(p);
        return parse_function_decl(p);
    }
    case KW_TRY: {
        advance(p);
        ATry *tr = tm_malloc(sizeof(ATry));
        memset(tr, 0, sizeof *tr);
        tr->body = parse_block(p);
        while (p->tok.kind == KW_CATCH) {
            advance(p);
            expect(p, P_LPAREN, "Expected '(' after catch");
            ACatch *ct = tm_malloc(sizeof(ACatch));
            memset(ct, 0, sizeof *ct);
            TStr **cls = NULL;
            int n = 0;
            for (;;) {
                cls = tm_realloc(cls, sizeof(TStr *) * (size_t)(n + 1));
                cls[n++] = parse_qual_name(p, NULL);
                if (!accept(p, P_PIPE)) break;
            }
            ct->cls = cls;
            ct->n_cls = n;
            if (p->tok.kind == L_VAR) {
                ct->var = p->tok.ident;
                advance(p);
            }
            expect(p, P_RPAREN, "Expected ')'");
            ct->body = parse_block(p);
            tr->catches = tm_realloc(tr->catches,
                                     sizeof(ACatch *) * (size_t)(tr->n_catches + 1));
            tr->catches[tr->n_catches++] = ct;
        }
        if (p->tok.kind == KW_FINALLY) {
            advance(p);
            tr->finally = parse_block(p);
        }
        if (tr->n_catches == 0 && !tr->finally)
            p_error(p, "try requires at least one catch or a finally block");
        AStmt *s = as_new(S_TRY, line);
        s->u.trys = tr;
        return s;
    }
    case KW_THROW: {
        advance(p);
        AExpr *e = parse_expr(p);
        expect(p, P_SEMI, "Expected ';' after throw");
        AStmt *s = as_new(S_THROW, line);
        s->u.e = e;
        return s;
    }
    case KW_GOTO: {
        advance(p);
        TStr *name = expect_ident(p, "Expected label after goto");
        expect(p, P_SEMI, "Expected ';'");
        AStmt *s = as_new(S_GOTO, line);
        s->u.name = name;
        return s;
    }
    case KW_CLASS:
    case KW_ABSTRACT:
    case KW_FINAL:
    case KW_READONLY:   /* P3: readonly class（PHP 8.2） */
    case KW_INTERFACE:
    case KW_TRAIT:
    case KW_ENUM: {
        uint8_t flags = 0;
        int kind = CK_CLASS;
        if (p->tok.kind == KW_ABSTRACT) { advance(p); flags |= CLS_ABSTRACT; }
        else if (p->tok.kind == KW_FINAL) { advance(p); flags |= CLS_FINAL; }
        else if (p->tok.kind == KW_READONLY) { advance(p); flags |= CLS_READONLY; }
        /* 修饰符可混排（final readonly / readonly final） */
        if (p->tok.kind == KW_ABSTRACT) { advance(p); flags |= CLS_ABSTRACT; }
        else if (p->tok.kind == KW_FINAL) { advance(p); flags |= CLS_FINAL; }
        else if (p->tok.kind == KW_READONLY) { advance(p); flags |= CLS_READONLY; }
        if (p->tok.kind == KW_CLASS) { advance(p); kind = CK_CLASS; }
        else if (p->tok.kind == KW_INTERFACE) { advance(p); kind = CK_INTERFACE; flags |= CLS_ABSTRACT; }
        else if (p->tok.kind == KW_TRAIT) { advance(p); kind = CK_TRAIT; }
        else if (p->tok.kind == KW_ENUM) { advance(p); kind = CK_ENUM; }
        else p_error(p, "Expected class-like declaration");
        return parse_class_decl(p, flags, kind, line);
    }
    case KW_DECLARE: {
        advance(p);
        expect(p, P_LPAREN, "Expected '('");
        while (p->tok.kind != P_RPAREN) advance(p);
        expect(p, P_RPAREN, "Expected ')'");
        return as_new(S_NOP, line); /* declare(...) is a no-op */
    }
    case KW_NAMESPACE: {
        /* `namespace A\B;` (rest of file) or `namespace A\B { ... }` or
         * the anonymous global block `namespace { ... }` */
        advance(p);
        if (p->tok.kind == P_SEMI) {         /* anonymous global scope */
            advance(p);
            AStmt *s = as_new(S_NS_BEGIN, line);
            s->u.name = tstr_intern("", 0);
            return s;
        }
        if (p->tok.kind == P_LBRACE) {       /* anonymous global block */
            advance(p);
            TStr *outer = p->ns;
            p->ns = tstr_intern("", 0);
            AStmtList *blk = asl_new();
            AStmt *beg = as_new(S_NS_BEGIN, line);
            beg->u.name = tstr_intern("", 0);
            asl_push(blk, beg);
            while (p->tok.kind != P_RBRACE) {
                if (p->tok.kind == L_EOF) p_error(p, "Unterminated namespace block");
                asl_push(blk, parse_statement(p));
            }
            expect(p, P_RBRACE, "Expected '}' to close namespace block");
            AStmt *end = as_new(S_NS_END, line);
            end->u.name = outer ? outer : tstr_intern("", 0);
            asl_push(blk, end);
            p->ns = outer;
            AStmt *b = as_new(S_BLOCK, line);
            b->u.stmts = blk;
            return b;
        }
        if (p->tok.kind == P_BACKSLASH)
            p_error(p, "Namespace declaration cannot be fully qualified");
        TStr *name = parse_qual_name(p, NULL);
        AStmt *s = as_new(S_NS_BEGIN, line);
        s->u.name = name;
        if (accept(p, P_SEMI)) { p->ns = name; return s; }
        expect(p, P_LBRACE, "Expected ';' or '{' after namespace declaration");
        TStr *outer = p->ns;
        p->ns = name;
        AStmtList *blk = asl_new();
        AStmt *beg = as_new(S_NS_BEGIN, line);
        beg->u.name = name;
        asl_push(blk, beg);
        while (p->tok.kind != P_RBRACE) {
            if (p->tok.kind == L_EOF) p_error(p, "Unterminated namespace block");
            asl_push(blk, parse_statement(p));
        }
        expect(p, P_RBRACE, "Expected '}' to close namespace block");
        AStmt *end = as_new(S_NS_END, line);
        end->u.name = outer ? outer : tstr_intern("", 0);
        asl_push(blk, end);
        p->ns = outer;
        AStmt *b = as_new(S_BLOCK, line);
        b->u.stmts = blk;
        return b;
    }
    case KW_USE: {
        /* use A\B\C; use A\B\C as D; use function f; use const K;
         * use A\B\{C, D as E};   —— 组 use（PHP 7+），组内 kind 统一 */
        advance(p);
        int kind = 0;
        if (p->tok.kind == KW_FUNCTION) { kind = 1; advance(p); }
        else if (p->tok.kind == KW_CONST) { kind = 2; advance(p); }
        AUseList *ul = tm_malloc(sizeof *ul);
        ul->kind = kind; ul->items = NULL; ul->n = 0;
        for (;;) {
            bool group = false;
            TStr *prefix = parse_use_prefix(p, &group);
            if (group) {
                advance(p);                     /* '{' */
                while (p->tok.kind != P_RBRACE) {
                    TStr *seg = parse_qual_name(p, NULL);
                    const char *ps = tstr_val(prefix);
                    const char *ss = tstr_val(seg);
                    size_t pl = strlen(ps), sl = strlen(ss);
                    char *full = tm_malloc(pl + sl + 2);
                    memcpy(full, ps, pl);
                    full[pl] = '\\';
                    memcpy(full + pl + 1, ss, sl);
                    AUseItem it = { .name = tstr_intern(full, pl + sl + 1),
                                    .alias = NULL };
                    tm_free(full);
                    if (accept(p, KW_AS)) {
                        if (p->tok.kind != L_IDENT)
                            p_error(p, "Expected alias name");
                        it.alias = p->tok.ident;
                        advance(p);
                    } else {
                        const char *s = tstr_val(it.name);
                        const char *last = strrchr(s, '\\');
                        it.alias = tstr_intern(last ? last + 1 : s,
                                               last ? strlen(last + 1) : strlen(s));
                    }
                    ul->items = tm_realloc(ul->items,
                                           sizeof(AUseItem) * (size_t)(ul->n + 1));
                    ul->items[ul->n++] = it;
                    if (!accept(p, P_COMMA)) break;
                }
                expect(p, P_RBRACE, "Expected '}' in group use");
            } else {
                AUseItem it = { .name = prefix, .alias = NULL };
                if (accept(p, KW_AS)) {
                    if (p->tok.kind != L_IDENT) p_error(p, "Expected alias name");
                    it.alias = p->tok.ident;
                    advance(p);
                } else {
                    /* default alias: last segment */
                    const char *s = tstr_val(it.name);
                    const char *last = strrchr(s, '\\');
                    it.alias = tstr_intern(last ? last + 1 : s,
                                           last ? strlen(last + 1) : strlen(s));
                }
                ul->items = tm_realloc(ul->items, sizeof(AUseItem) * (size_t)(ul->n + 1));
                ul->items[ul->n++] = it;
            }
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_SEMI, "Expected ';' after use statement");
        AStmt *s = as_new(S_USE, line);
        s->u.use = ul;
        return s;
    }
    case KW_REQUIRE:
    case KW_INCLUDE:
    case KW_REQUIRE_ONCE:
    case KW_INCLUDE_ONCE: {
        /* 字面量路径 → S_INCLUDE 编译期内联；动态路径 → S_EXPR + A_REQUIRE */
        bool is_req = p->tok.kind == KW_REQUIRE || p->tok.kind == KW_REQUIRE_ONCE;
        bool is_once = p->tok.kind == KW_REQUIRE_ONCE || p->tok.kind == KW_INCLUDE_ONCE;
        advance(p);
        AExpr *e = fold_str_concat(parse_expr(p));
        expect(p, P_SEMI, "Expected ';' after require/include");
        if (e->tag == A_STR) {
            AStmt *s = as_new(S_INCLUDE, line);
            s->u.inc.path = e->u.name;
            s->u.inc.path_expr = NULL;
            s->u.inc.is_require = is_req;
            s->u.inc.is_once = is_once;
            return s;
        }
        /* 动态路径：语句形态包成 S_EXPR + A_REQUIRE */
        AExpr *rq = ae_new(A_REQUIRE, line);
        rq->line = line;
        rq->u.inc.path = NULL;
        rq->u.inc.path_expr = e;
        rq->u.inc.is_require = is_req;
        rq->u.inc.is_once = is_once;
        AStmt *s = as_new(S_EXPR, line);
        s->u.e = rq;
        return s;
    }
    case KW_LIST: {
        ADestructList *dl = parse_destruct_list(p, false);
        expect(p, P_ASSIGN, "Expected '=' after list(...)");
        AExpr *rhs = parse_expr(p);
        expect(p, P_SEMI, "Expected ';'");
        AStmt *s = as_new(S_DESTRUCT, line);
        s->u.des.dl = dl;
        s->u.des.rhs = rhs;
        return s;
    }
    case P_LBRACKET: {
        ADestructList *dl = parse_destruct_list(p, true);
        expect(p, P_ASSIGN, "Expected '=' after destructuring pattern");
        AExpr *rhs = parse_expr(p);
        expect(p, P_SEMI, "Expected ';'");
        AStmt *s = as_new(S_DESTRUCT, line);
        s->u.des.dl = dl;
        s->u.des.rhs = rhs;
        return s;
    }
    default: {
        /* label lookahead: IDENT ':' at statement start */
        if (p->tok.kind == L_IDENT) {
            Lex saved = p->lx;
            Token saved_tok = p->tok;
            advance(p);
            if (p->tok.kind == P_COLON) {
                advance(p);
                AStmt *s = as_new(S_LABEL, line);
                s->u.name = saved_tok.ident;
                return s;
            }
            p->lx = saved;
            p->tok = saved_tok;
        }
        AExpr *e = parse_expr(p);
        expect(p, P_SEMI, "Expected ';'");
        AStmt *s = as_new(S_EXPR, line);
        s->u.e = e;
        return s;
    }
    }
}

/* ============ entry ============ */
AStmtList *tphp_parse_script(const char *src, const char *file) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.file = file;
    lex_init(&p.lx, src, file);
    advance(&p);
    AStmtList *l = asl_new();
    while (p.tok.kind != L_EOF) {
        if (p.tok.kind == KW_FUNCTION) {
            advance(&p);
            asl_push(l, parse_function_decl(&p));
            continue;
        }
        asl_push(l, parse_statement(&p));
    }
    return l;
}
