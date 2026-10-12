/*
 * TinyPHP.rt - a small PHP runtime
 * lex.h - tokenizer (reentrant; HTML/PHP mode switching; interpolation
 *         double-quoted strings are split into literal/expression parts)
 */
#ifndef TPHP_LEX_H
#define TPHP_LEX_H

#include "value.h"

typedef enum {
    L_EOF = 0, L_ERROR,
    /* literals */
    L_INT, L_FLOAT, L_STR, L_INLINE_HTML, L_IDENT, L_VAR,
    /* keywords (case-insensitive, PHP 8.5 set; unsupported ones parse-error) */
    KW_IF, KW_ELSE, KW_ELSEIF, KW_WHILE, KW_DO, KW_FOR, KW_FOREACH, KW_AS,
    KW_BREAK, KW_CONTINUE, KW_SWITCH, KW_CASE, KW_DEFAULT, KW_MATCH,
    KW_RETURN, KW_FUNCTION, KW_GLOBAL, KW_UNSET, KW_ECHO, KW_PRINT,
    KW_CONST, KW_NEW, KW_CLONE, KW_INSTANCEOF, KW_AND, KW_OR, KW_XOR,
    KW_TRUE, KW_FALSE, KW_NULL, KW_LIST, KW_ARRAY,
    KW_TRY, KW_CATCH, KW_FINALLY, KW_THROW, KW_STATIC, KW_EXIT, KW_DIE,
    KW_ISSET, KW_EMPTY, KW_DECLARE, KW_NAMESPACE, KW_USE,
    KW_REQUIRE, KW_INCLUDE, KW_FN, KW_GOTO, KW_CLASS,
    /* M3 OOP */
    KW_PUBLIC, KW_PROTECTED, KW_PRIVATE, KW_ABSTRACT, KW_FINAL,
    KW_INTERFACE, KW_TRAIT, KW_EXTENDS, KW_IMPLEMENTS, KW_READONLY,
    KW_ENUM, KW_SELF, KW_PARENT, KW_INSTEADOF,
    /* punctuation / operators */
    P_LPAREN, P_RPAREN, P_LBRACKET, P_RBRACKET, P_LBRACE, P_RBRACE,
    P_COMMA, P_SEMI, P_COLON, P_DOUBLE_COLON, P_FATARROW, P_OBJECT,
    P_NULLSAFE,
    P_PLUS, P_MINUS, P_MUL, P_DIV, P_MOD, P_POW, P_DOT,
    P_INC, P_DEC,
    P_ASSIGN, P_PLUS_EQ, P_MINUS_EQ, P_MUL_EQ, P_DIV_EQ, P_MOD_EQ,
    P_POW_EQ, P_DOT_EQ, P_AND_EQ, P_OR_EQ, P_XOR_EQ, P_SHL_EQ, P_SHR_EQ,
    P_COALESCE_EQ,
    P_EQ, P_NE, P_SEQ, P_SNE, P_LT, P_LE, P_GT, P_GE, P_SPACESHIP,
    P_ANDAND, P_OROR, P_NOT, P_AMP, P_PIPE, P_CARET, P_TILDE,
    P_SHL, P_SHR, P_QUESTION, P_COALESCE, P_ELLIPSIS, P_BACKSLASH, P_AT,
    P_PIPELINE, /* |> (PHP 8.5 pipe operator) */
} LKind;

/* a double-quoted string split into parts */
typedef struct {
    bool is_literal;      /* true: literal; false: expr source */
    union {
        TStr *lit;        /* is_literal */
        struct { const char *src; uint32_t len; int line; } expr; /* interpolation */
    } u;
} LexStrPart;

typedef struct {
    LKind kind;
    int line;
    int64_t i;
    double d;
    TStr *ident;          /* L_IDENT / L_VAR (without $) */
    LexStrPart *parts;    /* L_STR */
    int n_parts;
} Token;

typedef struct Lex {
    const char *src, *pos, *end;
    const char *file;
    int line;
    bool in_html;         /* currently emitting inline HTML */
} Lex;

void   lex_init(Lex *lx, const char *src, const char *file);
/* fragment variant: explicit end (interpolation parts live inside the
 * parent buffer and are NOT NUL-terminated — strlen would read past) */
void   lex_init_len(Lex *lx, const char *src, size_t len, const char *file);
Token  lex_next(Lex *lx);
/* token helpers */
const char *lex_kind_name(LKind k);

#endif /* TPHP_LEX_H */
