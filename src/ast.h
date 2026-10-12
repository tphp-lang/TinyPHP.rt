/*
 * TinyPHP.rt - a small PHP runtime
 * ast.h - tagged-union syntax tree produced by parse.c, consumed by compile.c
 *
 * M1 scope: procedural statements, arrays (full key semantics, destructuring,
 * spread), global aliasing, full operator set, string interpolation.
 */
#ifndef TPHP_AST_H
#define TPHP_AST_H

typedef struct AStmt AStmt;
typedef struct { AStmt **stmts; int n; } AStmtList;

#include "value.h"

typedef struct AExpr AExpr;
typedef struct AStmt AStmt;

/* ---- expressions ---- */
typedef enum {
    A_INT, A_FLOAT, A_STR,
    A_TRUE, A_FALSE, A_NULL,
    A_VAR,            /* u.name: variable */
    A_CONSTREF,       /* u.name: constant lookup (true/false/null handled above) */
    A_ARRAY,          /* u.alist */
    A_BINARY,         /* u.bin {op,l,r} */
    A_UNARY,          /* u.un {op,e} */
    A_ASSIGN,         /* u.bin {target l, value r} */
    A_ASSIGN_OP,      /* u.bin {op (P_PLUS_EQ..), target l, value r} */
    A_TERNARY,        /* u.tern {c, m(may be NULL for ?:), e} */
    A_COALESCE,       /* u.tern {c, e}: $a ?? $b */
    A_CALL,           /* u.call */
    A_DIM,            /* u.dim {base, idx (NULL for append $a[])} */
    A_CAST,           /* u.cast {ctype, e} */
    A_MATCH,          /* u.match */
    A_ISSET,          /* u.args */
    A_EMPTY,          /* u.args (single) */
    A_SPREAD,         /* u.e: ...$arr inside array literal */
    A_INCDEC,         /* u.un {op P_INC/P_DEC, e}; prefix flag in ext */
    A_CLOSURE,        /* u.clo {func, is_static}: function() use(..) {} */
    A_ARROW,          /* u.clo {func, is_static}: fn(params) => expr */
    A_NEW,            /* u.newx {name, args, anon}: new Exception(..) (built-in
                         ex / user classes); anon != NULL = new class {..} */
    A_GETCLASS,       /* u.e: expr::class (runtime: object or class-name string) */
    A_FIRSTCLASS,     /* u.name: f(...) (named function only, M2) */
    A_FIRSTCLASS_DYN, /* u.e: $f(...) / $obj->m(...) — value is closure or fn name */
    A_PIPELINE,       /* u.pipe {input, call}: $x |> f(args) */
    A_PROP,           /* u.prop {obj, name}: $e->message (exceptions in M2) */
    A_METHODCALL,     /* u.mcall {obj, name, args}: $e->getMessage() */
    A_STATICCALL,     /* u.scall {cls, member, args}: self::m() / C::m() */
    A_STATICGET,      /* u.scall {cls, member}: self::KONST (consts first) */
    A_INSTANCEOF,     /* u.bin {l: expr, r: A_CONSTREF class name} */
    A_CLONE,          /* u.e: clone $obj (calls __clone when defined) */
} ANTag;

typedef struct { AExpr *key; AExpr *val; bool is_spread; } AArrayItem;
typedef struct { AArrayItem *items; int n; } AArrayList;

typedef struct { AExpr **conds; int n_conds; AExpr *body; bool is_default; } AMatchArm;
typedef struct { AMatchArm *arms; int n; } AMatchList;

typedef struct { AExpr **items; int n; } AExprList;

/* call arguments carry name (named args) / spread markers */
typedef struct { AExpr *e; TStr *name; bool is_spread; } AArg;
typedef struct { AArg *items; int n; } AArgList;

typedef struct AParam {
    TStr *name;
    AExpr *default_val;   /* NULL when required */
    bool is_variadic;     /* ...$args (last param only) */
    bool is_promoted;     /* constructor property promotion (M3) */
    int vis;              /* CVIS_* when promoted */
    bool is_readonly;     /* promoted readonly */
} AParam;
typedef struct AFunc {
    TStr *name;           /* NULL for closures/arrows */
    AParam **params; int n_params;
    AExprList *uses;      /* closure `use ($a, $b)` — A_VAR exprs; NULL */
    bool is_static;
    AStmtList *body;
    int line;
} AFunc;
typedef struct ACatch {
    TStr **cls; int n_cls;   /* one or more exception class names */
    TStr *var;               /* NULL: catch without variable */
    AStmtList *body;
} ACatch;
typedef struct ATry {
    AStmtList *body;
    ACatch **catches; int n_catches;
    AStmtList *finally;      /* may be NULL */
} ATry;

typedef struct AExpr {
    int tag;
    int line;
    union {
        int64_t i;
        double d;
        TStr *name;
        AArrayList *alist;
        struct { int op; AExpr *l, *r; } bin;
        struct { int op; AExpr *e; bool prefix; } un;
        struct { AExpr *c, *m, *e; bool paren; } tern;
        struct { TStr *name; AExpr *dyn; AArgList *args; } call; /* name or dyn (variable fn) */
        struct { AExpr *base, *idx; } dim;
        struct { int ctype; AExpr *e; } cast;
        struct { AExpr *cond; AMatchList *arms; } match;
        AExprList *args;
        AExpr *e;
        struct { AFunc *func; bool is_static; } clo;
        struct { TStr *name; AArgList *args; void *anon; } newx;
        struct { AExpr *input; AExpr *call; } pipe;
        struct { AExpr *obj; TStr *name; } prop;
        struct { AExpr *obj; TStr *name; AArgList *args; } mcall;
        struct { TStr *cls; TStr *member; AArgList *args; } scall;
    } u;
} AExpr;

/* destructure target: variable slot or nested list; keys int/string/none */
typedef struct ADestructItem ADestructItem;
struct ADestructItem {
    TStr *str_key;         /* 'k' => target */
    int64_t int_key;
    bool has_key;
    AExpr *target;         /* A_VAR or nested A list target */
    ADestructItem **nested; /* when target is a nested list: items here */
    int n_nested;
};
typedef struct { ADestructItem **items; int n; bool is_list_syntax; } ADestructList;

/* ---- statements ---- */
typedef enum {
    S_ECHO,          /* u.exprs */
    S_EXPR,          /* u.e */
    S_IF,            /* u.ifs: chain */
    S_WHILE, S_DOWHILE, /* u.wh {cond, body} */
    S_FOR,           /* u.for */
    S_FOREACH,       /* u.fe */
    S_SWITCH,        /* u.sw */
    S_BREAK, S_CONTINUE, /* u.i: level */
    S_RETURN,        /* u.e (may be NULL) */
    S_GLOBAL,        /* u.names */
    S_UNSET,         /* u.args (A_DIM / A_VAR list) */
    S_DESTRUCT,      /* u.des {dl, rhs} */
    S_BLOCK,         /* u.stmts */
    S_FUNCDECL,      /* u.func */
    S_CONSTDECL,     /* u.consts */
    S_TRY,           /* u.trys */
    S_CLASSDECL,     /* u.cls */
    S_THROW,         /* u.e */
    S_GOTO, S_LABEL, /* u.name */
    S_NOP,
    /* namespaces (M3): u.name is the FQ namespace ("" = global);
     * S_NS_BEGIN may appear anywhere at statement level, S_NS_END restores */
    S_NS_BEGIN, S_NS_END,
    S_USE,           /* u.use: AUseList* */
} ASTag;





typedef struct { AExpr *cond; AStmtList *body; } AIfBranch;
typedef struct { AIfBranch *branches; int n; AStmtList *else_body; } AIfChain;

typedef struct { AExprList *init, *step; AExpr *cond; AStmtList *body; } AFor;
typedef struct { AExpr *arr; bool has_key; TStr *key_name; TStr *val_name; AStmtList *body; } AForeach;

typedef struct { AExpr *cond; AStmtList *body; bool is_default; } ACase;
typedef struct { AExpr *cond; ACase *cases; int n; } ASwitch;





typedef struct { TStr *name; AExpr *value; } AConstDecl;

/* ---- class declarations (M3) ----
 * A member is a tagged union; the class body is an ordered member list.
 * Methods reuse AFunc (same compile path as top-level functions). */
typedef struct AMod AMod;   /* trait adaptation: as/insteadof */
struct AMod {
    TStr *trait;            /* NULL = any */
    TStr *method;
    TStr *new_name;         /* `as NewName` (vis change allowed) */
    int new_vis;            /* -1 = unchanged */
    bool is_insteadof;
    TStr **from; int n_from;   /* insteadof trait list */
};
typedef struct { AMod *items; int n; } AModList;

typedef enum {
    CM_METHOD,       /* u.func (AFunc) */
    CM_PROP,         /* u.prop {name, def (AExpr|NULL)} */
    CM_CONST,        /* u.prop too */
    CM_CASE,         /* u.prop (enum) */
    CM_USE,          /* u.use {names, mods} */
} CMTag;

typedef struct AClassMember {
    int tag;
    int line;
    uint8_t vis;
    bool is_static, is_abstract, is_final, is_readonly;
    AFunc *func;
    struct { TStr *name; AExpr *def; } prop;
    struct { TStr **names; int n; AModList *mods; } use;
} AClassMember;

typedef struct { AClassMember **items; int n; } AClassBody;

/* file-level `use` import: kind 0 class / 1 function / 2 const */
typedef struct { TStr *name; TStr *alias; } AUseItem;
typedef struct {
    int kind;
    AUseItem *items; int n;
} AUseList;

typedef struct AClassDecl {
    TStr *name;             /* NULL = anonymous class */
    int kind;               /* ClassKind */
    uint8_t flags;          /* CLS_ABSTRACT | CLS_FINAL */
    TStr *parent;           /* NULL */
    TStr **ifaces; int n_ifaces;
    TStr *backing_marker;   /* enum: "int"/"string" backing type marker */
    AClassBody *body;
    int line;
} AClassDecl;

struct AStmt {
    int tag;
    int line;
    union {
        AExprList *exprs;     /* S_ECHO / S_UNSET */
        AExpr *e;
        AIfChain *ifs;
        struct { AExpr *cond; AStmtList *body; } wh;
        AFor *fors;
        AForeach *fe;
        ASwitch *sw;
        int64_t level;
        struct { TStr **names; int n; } names;  /* S_GLOBAL */
        AStmtList *stmts;     /* S_BLOCK */
        AFunc *func;
        struct { AConstDecl *decls; int n; } consts;
        struct { ADestructList *dl; AExpr *rhs; } des;  /* S_DESTRUCT */
        ATry *trys;           /* S_TRY */
        AClassDecl *cls;      /* S_CLASSDECL */
        TStr *name;           /* S_GOTO / S_LABEL / S_NS_BEGIN / S_NS_END */
        AUseList *use;        /* S_USE */
    } u;
};

/* parse a whole script; top-level function declarations stay in the tree */
AStmtList *tphp_parse_script(const char *src, const char *file);
#endif /* TPHP_AST_H */
