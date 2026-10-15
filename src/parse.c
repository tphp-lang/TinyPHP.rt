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

void tphp_fatal_src(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));


typedef struct {
    Lex lx;
    Token tok;       /* 1-token lookahead */
    const char *file;
    TStr *ns;        /* current namespace (S_NS_END carries the outer) */
} Parser;
static AExpr *parse_unary(Parser *p);
static AExpr *parse_closure(Parser *p, bool is_static);
static AExpr *parse_arrow_fn(Parser *p);


static AExpr *parse_expr(Parser *p);
static AExpr *parse_ternary(Parser *p);
static AStmt *parse_statement(Parser *p);
static AExpr *parse_static_access(Parser *p, TStr *name, int line);
static TStr **parse_name_list(Parser *p, int *out_n);
static AClassBody *parse_class_body(Parser *p, int kind);

/* ---- small helpers ---- */
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

static TStr *expect_ident(Parser *p, const char *what) {
    if (p->tok.kind != L_IDENT) p_error(p, what);
    TStr *name = p->tok.ident;
    advance(p);
    return name;
}

static AExpr *ae_new(int tag, int line) {
    AExpr *e = tm_malloc(sizeof(AExpr));
    memset(e, 0, sizeof *e);
    e->tag = tag;
    e->line = line;
    return e;
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
    case L_VAR: {
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
            return e;
        }
        if (p->tok.kind == P_DOUBLE_COLON) {
            /* C::member static access (primary returns here bypass postfix) */
            return parse_static_access(p, name, line);
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
            if (p->tok.kind == P_LPAREN) {
                args = parse_arg_list(p);
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
            cd->body = parse_class_body(p, CK_CLASS);
            AExpr *e = ae_new(A_NEW, line);
            e->u.newx.name = NULL;
            e->u.newx.args = args;
            e->u.newx.anon = cd;
            return e;
        }
        TStr *name = parse_qual_name(p, NULL);
        AArgList *args = parse_arg_list(p);
        AExpr *e = ae_new(A_NEW, line);
        e->u.newx.name = name;
        e->u.newx.args = args;
        return e;
    }
    case KW_CLONE: {
        /* clone <expr> — the operand is a unary-level expression */
        advance(p);
        AExpr *e = ae_new(A_CLONE, line);
        e->u.e = parse_unary(p);
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
         * backend can tell `C::$prop` apart from `C::CONST` */
        const char *v = tstr_val(p->tok.ident);
        size_t n = tstr_len(p->tok.ident);
        char *tmp = tm_malloc(n + 1);
        tmp[0] = '$';
        memcpy(tmp + 1, v, n);        /* content only: buffer is n+1 total */
        member = tstr_intern(tmp, n + 1);
        tm_free(tmp);
        advance(p);
    } else if (p->tok.kind == L_IDENT) {
        member = p->tok.ident;
        advance(p);
    } else if (p->tok.kind == KW_SELF || p->tok.kind == KW_PARENT ||
               p->tok.kind == KW_STATIC || p->tok.kind == KW_NEW) {
        /* keywords as member names: `Worker::self()`, `FFI::new()` are
         * valid static method calls in PHP */
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
            AArgList *args = parse_arg_list(p);
            AExpr *c = ae_new(A_CALL, line);
            c->u.call.dyn = e;
            c->u.call.args = args;
            e = c;
            break;
        }
        case P_OBJECT: {
            advance(p);
            if (p->tok.kind != L_IDENT &&
                p->tok.kind != KW_NEW &&
                p->tok.kind != KW_SELF &&
                p->tok.kind != KW_PARENT &&
                p->tok.kind != KW_STATIC &&
                p->tok.kind != KW_CLASS) {
                p_error(p, "Object property access lands in M3");
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
            if (p->tok.kind != L_IDENT &&
                p->tok.kind != KW_NEW &&
                p->tok.kind != KW_SELF &&
                p->tok.kind != KW_PARENT &&
                p->tok.kind != KW_STATIC) {
                p_error(p, "Object property access lands in M3");
            }
            TStr *name = p->tok.ident;
            advance(p);
            if (p->tok.kind == P_LPAREN) {
                AArgList *args = parse_arg_list(p);
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
             * (self::m()/parent::K/Static::m()/static::m()), and the
             * trailing `::class` after any value (Status::Active::class) */
            TStr *cls = NULL;
            if (e->tag == A_CONSTREF) {
                cls = e->u.name;
            } else {
                /* only `::class` may follow a value: Status::Active::class */
                advance(p);
                if (p->tok.kind != KW_CLASS)
                    p_error(p, "Only '::class' may follow an expression");
                advance(p);
                AExpr *gc = ae_new(A_GETCLASS, line);
                gc->u.e = e;
                e = gc;
                break;
            }
            advance(p);
            TStr *member;
            if (p->tok.kind == KW_CLASS) {
                member = tstr_intern("class", 5);
                advance(p);
            } else if (p->tok.kind == L_IDENT || p->tok.kind == KW_SELF ||
                       p->tok.kind == KW_PARENT || p->tok.kind == KW_STATIC) {
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
    { "array", 5 },
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
    return false;
}

static AExpr *parse_assign_level(Parser *p) {
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
static TStr *parse_type_decl(Parser *p) {
    bool nullable = accept(p, P_QUESTION);
    char buf[512];
    size_t n = 0;
    int segs = 0;
    for (;;) {
        const char *seg = NULL;
        char segbuf[256];
        if (p->tok.kind == L_IDENT) {
            /* qualified: A\B\C */
            size_t k = 0;
            const char *s = tstr_val(p->tok.ident);
            for (; s[k] && k < sizeof segbuf - 1; k++) segbuf[k] = s[k];
            segbuf[k] = 0;
            advance(p);
            while (p->tok.kind == P_BACKSLASH) {
                advance(p);
                if (p->tok.kind != L_IDENT) p_error(p, "Expected name after '\\'");
                size_t l = strlen(segbuf);
                if (l + 1 < sizeof segbuf) { segbuf[l] = '\\'; segbuf[l + 1] = 0; }
                const char *s2 = tstr_val(p->tok.ident);
                size_t m = strlen(segbuf), j = 0;
                for (; s2[j] && m + j < sizeof segbuf - 1; j++) segbuf[m + j] = s2[j];
                segbuf[m + j] = 0;
                advance(p);
            }
            /* lowercase builtin spellings (letters only; qualified names
             * containing '\' keep their declared spelling) */
            bool all_alpha = true;
            for (size_t j = 0; segbuf[j]; j++) {
                char ch = segbuf[j];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')))
                    { all_alpha = false; break; }
            }
            if (all_alpha) {
                for (size_t j = 0; segbuf[j]; j++)
                    if (segbuf[j] >= 'A' && segbuf[j] <= 'Z') segbuf[j] = (char)(segbuf[j] + 32);
            }
            seg = segbuf;
        } else if (p->tok.kind == KW_ARRAY) { seg = "array"; advance(p); }
        else if (p->tok.kind == KW_SELF) { seg = "self"; advance(p); }
        else if (p->tok.kind == KW_PARENT) { seg = "parent"; advance(p); }
        else if (p->tok.kind == KW_STATIC) { seg = "static"; advance(p); }
        else if (p->tok.kind == KW_NULL) { seg = "null"; advance(p); }
        else if (p->tok.kind == KW_TRUE) { seg = "true"; advance(p); }
        else if (p->tok.kind == KW_FALSE) { seg = "false"; advance(p); }
        else break;
        size_t sl = strlen(seg);
        if (n + (segs ? 1 : 0) + sl >= sizeof buf - 2)
            p_error(p, "Type declaration too long");
        if (segs) buf[n++] = '|';
        memcpy(buf + n, seg, sl);
        n += sl;
        segs++;
        if (!accept(p, P_PIPE)) break;
    }
    if (!segs) {
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

/* shared parameter list (named functions, closures, arrows) */
static void parse_params(Parser *p, AFunc *f) {
    while (p->tok.kind != P_RPAREN) {
        /* constructor promotion: visibility prefix before the type/param */
        int pvis = -1;
        bool pro_readonly = false;
        for (;;) {
            if (p->tok.kind == KW_PUBLIC) { advance(p); pvis = CVIS_PUBLIC; }
            else if (p->tok.kind == KW_PROTECTED) { advance(p); pvis = CVIS_PROTECTED; }
            else if (p->tok.kind == KW_PRIVATE) { advance(p); pvis = CVIS_PRIVATE; }
            else if (p->tok.kind == KW_READONLY) { advance(p); pro_readonly = true; }
            else break;
        }
        TStr *ptype = parse_type_decl(p);
        bool variadic = accept(p, P_ELLIPSIS);
        if (p->tok.kind != L_VAR) p_error(p, "Expected parameter variable");
        AParam *par = tm_malloc(sizeof(AParam));
        par->name = p->tok.ident;
        par->type = ptype;
        par->default_val = NULL;
        par->is_variadic = variadic;
        par->is_promoted = pvis >= 0 || pro_readonly;
        par->vis = pvis >= 0 ? pvis : CVIS_PUBLIC;
        par->is_readonly = pro_readonly;
        if ((pvis >= 0 || pro_readonly) && variadic)
            p_error(p, "Cannot promote a variadic parameter");
        advance(p);
        if (accept(p, P_ASSIGN)) par->default_val = parse_expr(p);
        f->params = tm_realloc(f->params, sizeof(AParam *) * (size_t)(f->n_params + 1));
        f->params[f->n_params++] = par;
        if (!accept(p, P_COMMA)) break;
    }
}

/* ============ class declarations (M3) ============ */
static AClassBody *parse_class_body(Parser *p, int kind);

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

static AClassMember *parse_class_member(Parser *p, int kind) {
    int line = p->tok.line;
    AClassMember *m = tm_malloc(sizeof(AClassMember));
    memset(m, 0, sizeof *m);
    m->line = line;
    m->vis = CVIS_PUBLIC;
    (void)kind;

    /* visibility / modifier prefix loop */
    for (;;) {
        if (p->tok.kind == KW_PUBLIC) { advance(p); m->vis = CVIS_PUBLIC; }
        else if (p->tok.kind == KW_PROTECTED) { advance(p); m->vis = CVIS_PROTECTED; }
        else if (p->tok.kind == KW_PRIVATE) { advance(p); m->vis = CVIS_PRIVATE; }
        else if (p->tok.kind == KW_STATIC) { advance(p); m->is_static = true; }
        else if (p->tok.kind == KW_ABSTRACT) { advance(p); m->is_abstract = true; }
        else if (p->tok.kind == KW_FINAL) { advance(p); m->is_final = true; }
        else if (p->tok.kind == KW_READONLY) { advance(p); m->is_readonly = true; }
        else break;
    }

    if (p->tok.kind == KW_FUNCTION) {
        advance(p);
        if (p->tok.kind != L_IDENT) p_error(p, "Expected method name");
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
        if (p->tok.kind == P_LBRACE) {
            if (m->is_abstract) p_error(p, "Abstract method cannot have a body");
            if (kind == CK_INTERFACE)    /* P3: 接口方法不允许有方法体 */
                p_error(p, "Interface method must not have a body");
            advance(p);
            AStmtList *l = asl_new();
            while (p->tok.kind != P_RBRACE) {
                if (p->tok.kind == L_EOF) p_error(p, "Unterminated method body");
                asl_push(l, parse_statement(p));
            }
            expect(p, P_RBRACE, "Expected '}'");
            f->body = l;
        } else {
            expect(p, P_SEMI, "Expected ';' or '{' after method signature");
            m->is_abstract = true;
        }
        return m;
    }

    if (p->tok.kind == KW_CONST) {
        advance(p);
        if (p->tok.kind != L_IDENT) p_error(p, "Expected constant name");
        m->tag = CM_CONST;
        m->prop.name = p->tok.ident;
        advance(p);
        expect(p, P_ASSIGN, "Expected '=' in class constant");
        m->prop.def = parse_expr(p);
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
                    if (p->tok.kind != L_IDENT) p_error(p, "Expected method name after '::'");
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

    /* property: [type] $var [= default] — P1-1 typed properties */
    if (p->tok.kind == L_VAR ||
        p->tok.kind == P_QUESTION || p->tok.kind == L_IDENT ||
        p->tok.kind == KW_ARRAY || p->tok.kind == KW_SELF ||
        p->tok.kind == KW_PARENT || p->tok.kind == KW_STATIC ||
        p->tok.kind == KW_NULL || p->tok.kind == KW_TRUE ||
        p->tok.kind == KW_FALSE) {
        TStr *ptype = parse_type_decl(p);
        if (p->tok.kind != L_VAR) p_error(p, "Expected property variable after type");
        if (kind == CK_INTERFACE)    /* P3: 接口不允许属性 */
            p_error(p, "Interfaces may not include properties");
        m->tag = CM_PROP;
        m->prop.name = p->tok.ident;
        m->prop.type = ptype;
        advance(p);
        if (accept(p, P_ASSIGN)) {
            m->prop.def = parse_expr(p);
        }
        expect(p, P_SEMI, "Expected ';'");
        return m;
    }

    p_error(p, "Unexpected token in class body");
    return NULL;
}

static AClassBody *parse_class_body(Parser *p, int kind) {
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
            if (accept(p, P_ASSIGN)) m->prop.def = parse_expr(p);
            expect(p, P_SEMI, "Expected ';'");
            b->items = tm_realloc(b->items, sizeof(AClassMember *) * (size_t)(b->n + 1));
            b->items[b->n++] = m;
            continue;
        }
        AClassMember *m = parse_class_member(p, kind);
        b->items = tm_realloc(b->items, sizeof(AClassMember *) * (size_t)(b->n + 1));
        b->items[b->n++] = m;
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
    cd->body = parse_class_body(p, kind);
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
    AStmtList *l = asl_new();
    while (p->tok.kind != P_RBRACE) {
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated closure body");
        asl_push(l, parse_statement(p));
    }
    expect(p, P_RBRACE, "Expected '}'");
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
    AStmtList *l = asl_new();
    AStmt *ret = as_new(S_RETURN, line);
    ret->u.e = parse_expr(p);
    asl_push(l, ret);
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
    AStmtList *l = asl_new();
    while (p->tok.kind != P_RBRACE) {
        if (p->tok.kind == L_EOF) p_error(p, "Unterminated function body");
        asl_push(l, parse_statement(p));
    }
    expect(p, P_RBRACE, "Expected '}'");
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
        /* use A\B\C; use A\B\C as D; use function f; use const K; */
        advance(p);
        int kind = 0;
        if (p->tok.kind == KW_FUNCTION) { kind = 1; advance(p); }
        else if (p->tok.kind == KW_CONST) { kind = 2; advance(p); }
        AUseList *ul = tm_malloc(sizeof *ul);
        ul->kind = kind; ul->items = NULL; ul->n = 0;
        for (;;) {
            AUseItem it = { .name = parse_qual_name(p, NULL), .alias = NULL };
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
            if (!accept(p, P_COMMA)) break;
        }
        expect(p, P_SEMI, "Expected ';' after use statement");
        AStmt *s = as_new(S_USE, line);
        s->u.use = ul;
        return s;
    }
    case KW_REQUIRE:
    case KW_INCLUDE: {
        /* M6: require/include —— 字符串字面量编译期内联；其他后置 */
        bool is_req = p->tok.kind == KW_REQUIRE;
        advance(p);
        AExpr *e = parse_expr(p);
        expect(p, P_SEMI, "Expected ';' after require/include");
        if (e->tag != A_STR)
            p_error(p, "require/include currently supports only string literal paths (M6)");
        AStmt *s = as_new(S_INCLUDE, line);
        s->u.inc.path = e->u.name;   /* A_STR 存在 u.name */
        s->u.inc.is_require = is_req;
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
