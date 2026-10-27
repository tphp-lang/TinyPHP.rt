/*
 * TinyPHP.rt - ffi.c - M5.5 FFI（libffi + LoadLibrary）
 *
 * 路线：PLAN/02 §4.5 txiki mod_ffi 精简 + php-src ext/ffi API 语义对齐。
 * cdef 子集：函数声明、基本类型、指针、平铺 struct/enum、简单 typedef；
 * 拒预处理指令（# 出现即 ValueError）。
 *
 * 对象模型：
 *   FFI        — cdef 返回的句柄对象；__handle = FFIScope 注册表下标
 *   FFI\CData  — 原生内存视图；__handle = FFCData 注册表下标
 *
 * 阻塞约定：ffi_call 是阻塞 native 调用，不挂起调度器——CPU 密集/长阻塞
 * 应放 Worker 线程。
 */
#include "tphp.h"
#include "value.h"
#include "opcode.h"
#include "class.h"
#include "tffi.h"
#include "pack.h"
#include <ffi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* ---- 类型枚举 ---- */
typedef enum {
    FFI_VOID, FFI_BOOL,
    FFI_INT8, FFI_UINT8, FFI_INT16, FFI_UINT16,
    FFI_INT32, FFI_UINT32, FFI_INT64, FFI_UINT64,
    FFI_FLOAT, FFI_DOUBLE,
    FFI_PTR, FFI_STRUCT, FFI_ARRAY, FFI_ENUM,
    FFI_WCHAR,          /* Windows: UTF-16 (2B)；Linux: UTF-32 (4B) */
    FFI_FUNC_PTR        /* 函数指针（回调签名；v1 仅参数位置） */
} FFICode;

typedef struct FFIType FFIType;
struct FFIType {
    FFICode code;
    size_t size, align;
    char *name;                       /* struct/enum/typedef 名，可空 */
    struct FFIType *array_elem;       /* PTR/ARRAY 元素类型 */
    size_t nelem;                     /* ARRAY 长度 */
    struct { char *name; struct FFIType *type; size_t offset; } *fields;
    int n_fields;
    bool is_union;                    /* union：全部字段 offset=0 */
    ffi_type *ffi;                    /* STRUCT 的 libffi 描述（惰性） */
    /* FFI_FUNC_PTR 签名（v1 回调）：返回类型 + 参数表；libffi 表示 = pointer */
    struct FFIType *fn_ret;
    struct FFIType **fn_params;
    int fn_n;
};

typedef struct FFIFunc {
    char *name;
    FFIType *ret;
    FFIType **params;
    int n_params;
    bool variadic;
    void *fn;                         /* 符号地址 */
    ffi_cif cif;
    ffi_type **cif_atypes;            /* heap: cif.arg_types 指向它，必须与
                                       * cif 同寿命（ffi_prep_cif 不拷贝数组；
                                       * 栈数组会在 cdef 返回后悬垂——ASAN
                                       * use-after-return 直接暴露） */
    void *sc;                         /* FFIScope*（延迟 resolve 用） */
    void *scope_lib;                  /* HMODULE 备份（resolve_symbol） */
} FFIFunc;

typedef struct FFIScope {
    void *lib;
    FFIType **types; int n_types, cap_types;
    FFIFunc **funcs; int n_funcs, cap_funcs;
    struct { char *name; int64_t val; } *consts;
    int n_consts, cap_consts;
} FFIScope;

typedef struct FFCData {
    void *ptr;
    FFIType *type;
    bool owned;
    FFIScope *scope;
} FFCData;

/* ---- 类指针（每线程） ---- */
static _Thread_local TClass *g_ffi_cls;
static _Thread_local TClass *g_cdata_cls;

/* ---- 注册表 ---- */
static _Thread_local FFIScope **g_scopes;
static _Thread_local int g_n_scopes, g_cap_scopes;
static _Thread_local FFCData **g_cdatas;
static _Thread_local int g_n_cdatas, g_cap_cdatas;

static int scope_reg(FFIScope *s) {
    if (g_n_scopes >= g_cap_scopes) {
        g_cap_scopes = g_cap_scopes ? g_cap_scopes * 2 : 8;
        g_scopes = tm_realloc(g_scopes, sizeof(FFIScope *) * (size_t)g_cap_scopes);
    }
    g_scopes[g_n_scopes] = s;
    return g_n_scopes++;
}

static int cdata_reg(FFCData *d) {
    if (g_n_cdatas >= g_cap_cdatas) {
        g_cap_cdatas = g_cap_cdatas ? g_cap_cdatas * 2 : 32;
        g_cdatas = tm_realloc(g_cdatas, sizeof(FFCData *) * (size_t)g_cap_cdatas);
    }
    g_cdatas[g_n_cdatas] = d;
    return g_n_cdatas++;
}

/* ---- 内建类型（进程级只读） ---- */
static FFIType *bt_void, *bt_bool;
static FFIType *bt_i8, *bt_u8, *bt_i16, *bt_u16;
static FFIType *bt_i32, *bt_u32, *bt_i64, *bt_u64;
static FFIType *bt_f32, *bt_f64;
static FFIType *bt_ptr;
static FFIType *bt_long, *bt_ulong, *bt_llong, *bt_ullong;
static FFIType *bt_size, *bt_ssize, *bt_iptr, *bt_uptr;
static FFIType *bt_wchar;

static FFIType *type_new(FFICode code, size_t size, size_t align) {
    FFIType *t = tm_malloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->code = code;
    t->size = size;
    t->align = align;
    return t;
}

static void builtin_types_init(void) {
    if (bt_void) return;
    bt_void  = type_new(FFI_VOID, 0, 1);
    bt_bool  = type_new(FFI_BOOL, 1, 1);
    bt_i8    = type_new(FFI_INT8, 1, 1);
    bt_u8    = type_new(FFI_UINT8, 1, 1);
    bt_i16   = type_new(FFI_INT16, 2, 2);
    bt_u16   = type_new(FFI_UINT16, 2, 2);
    bt_i32   = type_new(FFI_INT32, 4, 4);
    bt_u32   = type_new(FFI_UINT32, 4, 4);
    bt_i64   = type_new(FFI_INT64, 8, 8);
    bt_u64   = type_new(FFI_UINT64, 8, 8);
    bt_f32   = type_new(FFI_FLOAT, 4, 4);
    bt_f64   = type_new(FFI_DOUBLE, 8, 8);
    bt_ptr   = type_new(FFI_PTR, sizeof(void *), sizeof(void *));
    bt_long  = type_new(FFI_INT32, 4, 4);    /* Windows LLP64 */
    bt_ulong = type_new(FFI_UINT32, 4, 4);
    bt_llong  = type_new(FFI_INT64, 8, 8);
    bt_ullong = type_new(FFI_UINT64, 8, 8);
    bt_size  = bt_u64;
    bt_ssize = bt_i64;
    bt_iptr  = bt_i64;
    bt_uptr  = bt_u64;
#ifdef _WIN32
    bt_wchar = type_new(FFI_WCHAR, 2, 2);    /* UTF-16 */
#else
    bt_wchar = type_new(FFI_WCHAR, 4, 4);    /* UTF-32 */
#endif
}

static ffi_type *to_ffi_type(FFIType *t) {
    switch (t->code) {
    case FFI_VOID:  return &ffi_type_void;
    case FFI_BOOL:  return &ffi_type_uint8;
    case FFI_INT8:  return &ffi_type_sint8;
    case FFI_UINT8: return &ffi_type_uint8;
    case FFI_INT16: return &ffi_type_sint16;
    case FFI_UINT16:return &ffi_type_uint16;
    case FFI_INT32: return &ffi_type_sint32;
    case FFI_UINT32:return &ffi_type_uint32;
    case FFI_INT64: return &ffi_type_sint64;
    case FFI_UINT64:return &ffi_type_uint64;
    case FFI_FLOAT: return &ffi_type_float;
    case FFI_DOUBLE:return &ffi_type_double;
    case FFI_PTR:   return &ffi_type_pointer;
    case FFI_FUNC_PTR: return &ffi_type_pointer;
    case FFI_ENUM:  return &ffi_type_sint32;
    case FFI_WCHAR:
#ifdef _WIN32
        return &ffi_type_uint16;
#else
        return &ffi_type_uint32;
#endif
    case FFI_STRUCT: case FFI_ARRAY: return t->ffi;
    }
    return &ffi_type_void;
}

static void prepare_struct_ffi(FFIType *t) {
    if (t->ffi) return;
    if (t->is_union) {
        /* libffi 无原生 union：按值传参用最大成员的 ffi_type（x64 上
         * 小 union 与最大标量同寄存器约定；CData 字段访问不受影响） */
        FFIType *big = NULL;
        for (int i = 0; i < t->n_fields; i++) {
            FFIType *fm = t->fields[i].type;
            if (fm->code == FFI_STRUCT || fm->code == FFI_ARRAY)
                prepare_struct_ffi(fm);
            if (!big || fm->size > big->size) big = fm;
        }
        if (big) {
            if (big->code == FFI_STRUCT || big->code == FFI_ARRAY)
                t->ffi = big->ffi;
            else
                t->ffi = to_ffi_type(big);
            /* 标记已准备（借用指针，不 free） */
            return;
        }
        t->ffi = &ffi_type_uint64;
        return;
    }
    ffi_type *ft = tm_malloc(sizeof *ft);
    memset(ft, 0, sizeof *ft);
    ft->type = FFI_TYPE_STRUCT;
    int n = t->n_fields;
    ft->elements = tm_malloc(sizeof(ffi_type *) * (size_t)(n + 1));
    for (int i = 0; i < n; i++) {
        FFIType *fm = t->fields[i].type;
        if (fm->code == FFI_STRUCT) prepare_struct_ffi(fm);
        ft->elements[i] = to_ffi_type(fm);
    }
    ft->elements[n] = NULL;
    ffi_get_struct_offsets(FFI_DEFAULT_ABI, ft, NULL);
    t->ffi = ft;
    if (ft->size) t->size = ft->size;
    if (ft->alignment) t->align = ft->alignment;
}

/* ---- cdef 解析器 ---- */

typedef struct {
    const char *s, *p, *end;
    char tok[256];
    int tkind;   /* 0=eof 1=ident 2=num 3=punct */
} CParser;

static void cskip(CParser *cp) {
    for (;;) {
        while (cp->p < cp->end && isspace((unsigned char)*cp->p)) cp->p++;
        if (cp->p + 1 < cp->end && cp->p[0] == '/' && cp->p[1] == '/') {
            while (cp->p < cp->end && *cp->p != '\n') cp->p++;
            continue;
        }
        if (cp->p + 1 < cp->end && cp->p[0] == '/' && cp->p[1] == '*') {
            cp->p += 2;
            while (cp->p + 1 < cp->end && !(cp->p[0] == '*' && cp->p[1] == '/')) cp->p++;
            if (cp->p + 1 < cp->end) cp->p += 2;
            continue;
        }
        break;
    }
}

static int cnext(CParser *cp) {
    cskip(cp);
    if (cp->p >= cp->end) { cp->tkind = 0; cp->tok[0] = 0; return 0; }
    char c = *cp->p;
    if (isalpha((unsigned char)c) || c == '_') {
        int n = 0;
        while (cp->p < cp->end && (isalnum((unsigned char)*cp->p) || *cp->p == '_')) {
            if (n < 255) cp->tok[n++] = *cp->p;
            cp->p++;
        }
        cp->tok[n] = 0; cp->tkind = 1; return 1;
    }
    if (isdigit((unsigned char)c)) {
        int n = 0;
        while (cp->p < cp->end && (isalnum((unsigned char)*cp->p) || *cp->p == '.')) {
            if (n < 255) cp->tok[n++] = *cp->p;
            cp->p++;
        }
        cp->tok[n] = 0; cp->tkind = 2; return 2;
    }
    if (cp->p + 2 < cp->end && cp->p[0] == '.' && cp->p[1] == '.' && cp->p[2] == '.') {
        cp->tok[0] = '.'; cp->tok[1] = '.'; cp->tok[2] = '.'; cp->tok[3] = 0;
        cp->p += 3; cp->tkind = 3; return 3;
    }
    if (cp->p + 1 < cp->end) {
        char c2 = cp->p[1];
        if ((c == '*' || c == '&' || c == '<' || c == '>' || c == '=' ||
             c == '!' || c == '+' || c == '-') && c2 == '=') {
            cp->tok[0] = c; cp->tok[1] = c2; cp->tok[2] = 0;
            cp->p += 2; cp->tkind = 3; return 3;
        }
    }
    cp->tok[0] = c; cp->tok[1] = 0;
    cp->p++; cp->tkind = 3; return 3;
}

static int cis(CParser *cp, const char *s) {
    return cp->tkind != 0 && strcmp(cp->tok, s) == 0;
}

static void cexpect(CParser *cp, const char *s) {
    if (!cis(cp, s))
        tphp_raise("ValueError", "FFI::cdef(): expected \"%s\" near \"%s\"", s, cp->tok);
    cnext(cp);
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = tm_malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static void check_no_preproc(const char *code) {
    const char *p = code;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#')
            tphp_raise("ValueError",
                       "FFI API doesn't support C preprocessor syntax");
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

static FFIType *parse_type(CParser *cp, FFIScope *sc);
static FFIType *parse_declarator(CParser *cp, FFIScope *sc, FFIType *base,
                                 char *name_out, size_t name_cap,
                                 bool unnamed_ok);
static void parse_param_list(CParser *cp, FFIScope *sc,
                             FFIType **params, int max, int *np, bool *variadic);
static FFIType *parse_named_body(CParser *cp, FFIScope *sc, bool is_enum,
                                 const char *sname);

/* parse_type 入口：struct/union/enum 转发；已命名类型若无 { 则查表引用 */
static FFIType *parse_struct_body(CParser *cp, FFIScope *sc, bool is_enum) {
    /* is_enum=false 时可能被 parse_type 以 struct|union 进入；
     * union 标志由调用方通过 cis 消费后的状态无法回传——统一：
     * parse_type 在调用前已消费 struct/union/enum 关键字。 */
    if (!cis(cp, "{") && cp->tkind == 1) {
        char nbuf[128];
        snprintf(nbuf, sizeof nbuf, "%s", cp->tok);
        cnext(cp);
        if (cis(cp, "{"))
            return parse_named_body(cp, sc, is_enum, nbuf);
        /* sc==NULL（FFI::new 静态调用）时无注册表可查——明确报错，
         * 不允许 NULL 解引用（v2：曾 AV） */
        if (!sc)
            tphp_raise("ValueError",
                       "FFI: struct/union/enum \"%s\" needs an FFI instance "
                       "scope (use $ffi->new / $ffi->type); FFI::new() "
                       "static call cannot see cdef types", nbuf);
        for (int i = 0; i < sc->n_types; i++)
            if (sc->types[i]->name && strcmp(sc->types[i]->name, nbuf) == 0)
                return sc->types[i];
        tphp_raise("ValueError", "FFI::cdef(): unknown type \"%s\"", nbuf);
    }
    return parse_named_body(cp, sc, is_enum, NULL);
}

static FFIType *apply_pointers(CParser *cp, FFIType *t) {
    while (cp->tkind == 3 && cp->tok[0] == '*') {
        cnext(cp);
        while (cis(cp, "const") || cis(cp, "volatile") || cis(cp, "restrict"))
            cnext(cp);
        FFIType *p = type_new(FFI_PTR, sizeof(void *), sizeof(void *));
        p->array_elem = t;
        t = p;
    }
    return t;
}

static void scope_add_type(FFIScope *sc, FFIType *t) {
    if (sc->n_types >= sc->cap_types) {
        sc->cap_types = sc->cap_types ? sc->cap_types * 2 : 16;
        sc->types = tm_realloc(sc->types, sizeof(FFIType *) * (size_t)sc->cap_types);
    }
    sc->types[sc->n_types++] = t;
}

static void scope_add_const(FFIScope *sc, const char *name, int64_t val) {
    if (sc->n_consts >= sc->cap_consts) {
        sc->cap_consts = sc->cap_consts ? sc->cap_consts * 2 : 16;
        sc->consts = tm_realloc(sc->consts, sizeof(*sc->consts) * (size_t)sc->cap_consts);
    }
    sc->consts[sc->n_consts].name = xstrdup(name);
    sc->consts[sc->n_consts].val = val;
    sc->n_consts++;
}

static FFIType *lookup_type_name(CParser *cp, FFIScope *sc) {
    const char *n = cp->tok;
    if (strcmp(n, "void") == 0)  { cnext(cp); return bt_void; }
    if (strcmp(n, "bool") == 0 || strcmp(n, "_Bool") == 0) { cnext(cp); return bt_bool; }
    if (strcmp(n, "char") == 0)  { cnext(cp); return bt_i8; }
    if (strcmp(n, "signed") == 0) {
        cnext(cp);
        if (cis(cp, "char")) { cnext(cp); return bt_i8; }
        if (cis(cp, "short")) { cnext(cp); if (cis(cp, "int")) cnext(cp); return bt_i16; }
        if (cis(cp, "long")) {
            cnext(cp);
            if (cis(cp, "long")) { cnext(cp); if (cis(cp, "int")) cnext(cp); return bt_llong; }
            if (cis(cp, "int")) cnext(cp);
            return bt_long;
        }
        if (cis(cp, "int")) { cnext(cp); return bt_i32; }
        return bt_i32;
    }
    if (strcmp(n, "unsigned") == 0) {
        cnext(cp);
        if (cis(cp, "char")) { cnext(cp); return bt_u8; }
        if (cis(cp, "short")) { cnext(cp); if (cis(cp, "int")) cnext(cp); return bt_u16; }
        if (cis(cp, "long")) {
            cnext(cp);
            if (cis(cp, "long")) { cnext(cp); if (cis(cp, "int")) cnext(cp); return bt_ullong; }
            if (cis(cp, "int")) cnext(cp);
            return bt_ulong;
        }
        if (cis(cp, "int")) { cnext(cp); return bt_u32; }
        return bt_u32;
    }
    if (strcmp(n, "short") == 0) { cnext(cp); if (cis(cp, "int")) cnext(cp); return bt_i16; }
    if (strcmp(n, "int") == 0)   { cnext(cp); return bt_i32; }
    if (strcmp(n, "long") == 0) {
        cnext(cp);
        if (cis(cp, "long")) { cnext(cp); if (cis(cp, "int")) cnext(cp); return bt_llong; }
        if (cis(cp, "int")) cnext(cp);
        return bt_long;
    }
    if (strcmp(n, "float") == 0)  { cnext(cp); return bt_f32; }
    if (strcmp(n, "double") == 0) {
        cnext(cp);
        if (cis(cp, "long")) cnext(cp);
        return bt_f64;
    }
    if (strcmp(n, "size_t") == 0)   { cnext(cp); return bt_size; }
    if (strcmp(n, "ssize_t") == 0)  { cnext(cp); return bt_ssize; }
    if (strcmp(n, "intptr_t") == 0) { cnext(cp); return bt_iptr; }
    if (strcmp(n, "uintptr_t") == 0){ cnext(cp); return bt_uptr; }
    if (strcmp(n, "int8_t") == 0)   { cnext(cp); return bt_i8; }
    if (strcmp(n, "uint8_t") == 0)  { cnext(cp); return bt_u8; }
    if (strcmp(n, "int16_t") == 0)  { cnext(cp); return bt_i16; }
    if (strcmp(n, "uint16_t") == 0) { cnext(cp); return bt_u16; }
    if (strcmp(n, "int32_t") == 0)  { cnext(cp); return bt_i32; }
    if (strcmp(n, "uint32_t") == 0) { cnext(cp); return bt_u32; }
    if (strcmp(n, "int64_t") == 0)  { cnext(cp); return bt_i64; }
    if (strcmp(n, "uint64_t") == 0) { cnext(cp); return bt_u64; }
    if (strcmp(n, "wchar_t") == 0)  { cnext(cp); return bt_wchar; }
    if (strcmp(n, "WCHAR") == 0 || strcmp(n, "wchar") == 0) {
        cnext(cp); return bt_wchar;
    }
    if (sc) {
        for (int i = 0; i < sc->n_types; i++) {
            if (sc->types[i]->name && strcmp(sc->types[i]->name, n) == 0) {
                FFIType *t = sc->types[i];
                cnext(cp);
                return t;
            }
        }
    }
    return NULL;
}

static FFIType *parse_declarator(CParser *cp, FFIScope *sc, FFIType *base,
                                 char *name_out, size_t name_cap,
                                 bool unnamed_ok) {
    (void)sc;
    FFIType *t = apply_pointers(cp, base);
    /* 函数指针：(*name)(params) —— base 是返回类型；解析完整签名存入
     * FFI_FUNC_PTR（v1 回调：闭包传入该位置时建 libffi trampoline） */
    if (cp->tkind == 3 && cp->tok[0] == '(') {
        const char *save = cp->p;
        int skind = cp->tkind;
        char stok[256];
        memcpy(stok, cp->tok, sizeof stok);
        cnext(cp);
        if (cp->tkind == 3 && cp->tok[0] == '*') {
            cnext(cp);
            if (cp->tkind == 1) {
                if (name_out && name_cap) snprintf(name_out, name_cap, "%s", cp->tok);
                cnext(cp);
                if (cp->tkind == 3 && cp->tok[0] == ')') {
                    cnext(cp);
                    if (cis(cp, "(")) {
                        cnext(cp);
                        FFIType *fp = type_new(FFI_FUNC_PTR,
                                               sizeof(void *), sizeof(void *));
                        fp->fn_ret = t;          /* base = 返回类型 */
                        FFIType *ps[32];
                        int nps = 0;
                        bool varg = false;
                        if (!cis(cp, ")"))
                            parse_param_list(cp, sc, ps, 32, &nps, &varg);
                        else
                            cnext(cp);
                        fp->fn_n = nps;
                        fp->fn_params = nps
                            ? tm_malloc(sizeof(FFIType *) * (size_t)nps) : NULL;
                        for (int i = 0; i < nps; i++) fp->fn_params[i] = ps[i];
                        return fp;
                    }
                }
            }
        }
        /* 不是函数指针：回退重解析 */
        cp->p = save; cp->tkind = skind;
        memcpy(cp->tok, stok, sizeof cp->tok);
        t = apply_pointers(cp, base);
    }
    /* 无名参数（仅参数位允许）：(int) / (const char *) */
    if (unnamed_ok && cp->tkind == 3 && (cp->tok[0] == ',' || cp->tok[0] == ')')) {
        if (name_out && name_cap) name_out[0] = 0;
        return t;
    }
    /* cdef 路径：名字必填；类型表达式路径由 parse_type_expr 单独处理 */
    if (cp->tkind != 1)
        tphp_raise("ValueError",
                   "FFI::cdef(): expected identifier in declarator, got \"%s\"", cp->tok);
    if (name_out && name_cap)
        snprintf(name_out, name_cap, "%s", cp->tok);
    cnext(cp);
    /* bitfield: name : N —— 明确拒绝 */
    if (cp->tkind == 3 && cp->tok[0] == ':')
        tphp_raise("ValueError", "FFI::cdef(): bitfields are not supported");
    while (cis(cp, "[")) {
        cnext(cp);
        size_t n = 1;
        if (cp->tkind == 2) {
            n = (size_t)strtoull(cp->tok, NULL, 0);
            cnext(cp);
        }
        cexpect(cp, "]");
        FFIType *at = type_new(FFI_ARRAY, t->size * n, t->align);
        at->array_elem = t;
        at->nelem = n;
        t = at;
    }
    return t;
}

/* 参数表（函数声明与函数指针签名共用）：进入时已消费 "("，返回时已消费 ")"。
 * 数组参数按 C 规则退化为指针。 */
static void parse_param_list(CParser *cp, FFIScope *sc,
                             FFIType **params, int max, int *np, bool *variadic) {
    *np = 0;
    *variadic = false;
    for (;;) {
        if (cis(cp, "...")) {
            *variadic = true;
            cnext(cp);
            break;
        }
        FFIType *pb = parse_type(cp, sc);
        if (!pb)
            tphp_raise("ValueError", "FFI::cdef(): bad parameter type near \"%s\"", cp->tok);
        char pn[128] = {0};
        FFIType *pt = parse_declarator(cp, sc, pb, pn, sizeof pn, true);
        if (pt->code == FFI_ARRAY) {
            FFIType *p = type_new(FFI_PTR, sizeof(void *), sizeof(void *));
            p->array_elem = pt->array_elem;
            pt = p;
        }
        if (*np < max) params[(*np)++] = pt;
        if (cis(cp, ",")) { cnext(cp); continue; }
        break;
    }
    cexpect(cp, ")");
}

static FFIType *parse_type(CParser *cp, FFIScope *sc) {
    while (cis(cp, "const") || cis(cp, "volatile") || cis(cp, "restrict") ||
           cis(cp, "static") || cis(cp, "extern") || cis(cp, "inline"))
        cnext(cp);
    if (cis(cp, "struct")) {
        cnext(cp);
        return parse_struct_body(cp, sc, false);
    }
    if (cis(cp, "union")) {
        cnext(cp);
        FFIType *t = parse_struct_body(cp, sc, false);
        if (t && t->code == FFI_STRUCT && !t->is_union) {
            /* 重算 union 布局：全部 offset=0 */
            t->is_union = true;
            size_t maxs = 1, maxa = 1;
            for (int i = 0; i < t->n_fields; i++) {
                t->fields[i].offset = 0;
                if (t->fields[i].type->size > maxs) maxs = t->fields[i].type->size;
                if (t->fields[i].type->align > maxa) maxa = t->fields[i].type->align;
            }
            t->align = maxa;
            t->size = (maxs + maxa - 1) & ~(maxa - 1);
            if (t->size == 0) t->size = 1;
            /* prepare_struct_ffi 若已建需重建 */
            if (t->ffi) {
                if (t->ffi->elements) tm_free(t->ffi->elements);
                tm_free(t->ffi);
                t->ffi = NULL;
            }
        }
        return t;
    }
    if (cis(cp, "enum")) {
        cnext(cp);
        return parse_struct_body(cp, sc, true);
    }
    if (cp->tkind != 1) return NULL;
    return lookup_type_name(cp, sc);
}

static void *resolve_symbol(FFIScope *sc, const char *name) {
#ifdef _WIN32
    if (sc && sc->lib) {
        void *p = (void *)GetProcAddress((HMODULE)sc->lib, name);
        if (p) return p;
    }
    static const char *fb[] = {
        "kernel32.dll", "user32.dll", "msvcrt.dll", "advapi32.dll",
        "shell32.dll", "ws2_32.dll", "ole32.dll", "gdi32.dll", NULL
    };
    for (int i = 0; fb[i]; i++) {
        HMODULE h = GetModuleHandleA(fb[i]);
        if (!h) h = LoadLibraryA(fb[i]);
        if (!h) continue;
        void *p = (void *)GetProcAddress(h, name);
        if (p) return p;
    }
    HMODULE exe = GetModuleHandleA(NULL);
    if (exe) {
        void *p = (void *)GetProcAddress(exe, name);
        if (p) return p;
    }
    return NULL;
#else
    if (sc && sc->lib) {
        void *p = dlsym(sc->lib, name);
        if (p) return p;
    }
    /* 已加载的全局符号表（RTLD_DEFAULT） */
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p) return p;
    /* 常见 libc 路径兜底 */
    static const char *fb[] = {
        "libc.so.6", "libc.so", "libm.so.6", "libdl.so.2", NULL
    };
    for (int i = 0; fb[i]; i++) {
        void *h = dlopen(fb[i], RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen(fb[i], RTLD_LAZY);
        if (!h) continue;
        p = dlsym(h, name);
        if (p) return p;
    }
    return NULL;
#endif
}

/* 变参固定前缀后的额外 cif */
typedef struct {
    ffi_cif cif;
    ffi_type *atypes[16];
    bool ready;
} VarCif;

/* 简化：每次变参调用重建（额外参数 ≤12） */

static void parse_function(CParser *cp, FFIScope *sc, FFIType *ret, const char *fname) {
    if (!cis(cp, "("))
        tphp_raise("ValueError", "FFI::cdef(): expected \"(\" after function name");
    cnext(cp);

    FFIType *params[32];
    int np = 0;
    bool variadic = false;
    if (!cis(cp, ")")) {
        const char *save = cp->p;
        int skind = cp->tkind;
        char stok[256];
        memcpy(stok, cp->tok, sizeof stok);
        if (cis(cp, "void")) {
            cnext(cp);
            if (cis(cp, ")")) {
                cnext(cp);          /* 消费 ) */
                goto params_semicolon;
            }
            cp->p = save; cp->tkind = skind;
            memcpy(cp->tok, stok, sizeof cp->tok);
        }
        parse_param_list(cp, sc, params, 32, &np, &variadic);
    } else {
        cnext(cp);   /* 空参数表 () */
    }
params_semicolon:
    cexpect(cp, ";");

    FFIFunc *fn = tm_malloc(sizeof *fn);
    memset(fn, 0, sizeof *fn);
    fn->name = xstrdup(fname);
    fn->ret = ret;
    fn->n_params = np;
    fn->variadic = variadic;
    fn->sc = sc;
    fn->scope_lib = sc ? sc->lib : NULL;
    fn->params = np ? tm_malloc(sizeof(FFIType *) * (size_t)np) : NULL;
    for (int i = 0; i < np; i++) fn->params[i] = params[i];

    /* ffi_prep_cif 只保存 arg_types 指针、不拷贝——必须堆分配并与 cif
     * 同寿命。栈数组在 parse_function 返回后悬垂（普通构建靠运气活着，
     * ASAN use-after-return / 栈复用即崩：M5.5 FFI 调用类 0xC0000005 根因）。 */
    ffi_type **atypes = tm_malloc(sizeof(ffi_type *) * (np > 0 ? (size_t)np : 1));
    fn->cif_atypes = atypes;
    for (int i = 0; i < np; i++) {
        if (params[i]->code == FFI_STRUCT) prepare_struct_ffi(params[i]);
        atypes[i] = to_ffi_type(params[i]);
    }
    if (ret->code == FFI_STRUCT) prepare_struct_ffi(ret);
    ffi_status st = ffi_prep_cif(&fn->cif, FFI_DEFAULT_ABI,
                                 (unsigned)np, to_ffi_type(ret), atypes);
    if (st != FFI_OK)
        tphp_raise("ValueError", "FFI::cdef(): ffi_prep_cif failed for %s", fname);

    fn->fn = resolve_symbol(sc, fname);

    if (sc->n_funcs >= sc->cap_funcs) {
        sc->cap_funcs = sc->cap_funcs ? sc->cap_funcs * 2 : 16;
        sc->funcs = tm_realloc(sc->funcs, sizeof(FFIFunc *) * (size_t)sc->cap_funcs);
    }
    sc->funcs[sc->n_funcs++] = fn;
}

static void parse_toplevel(CParser *cp, FFIScope *sc) {
    builtin_types_init();
    while (cp->tkind != 0) {
        if (cis(cp, "typedef")) {
            cnext(cp);
            FFIType *base = parse_type(cp, sc);
            if (!base)
                tphp_raise("ValueError", "FFI::cdef(): bad typedef");
            FFIType *t = apply_pointers(cp, base);
            if (cp->tkind != 1)
                tphp_raise("ValueError", "FFI::cdef(): typedef needs a name");
            char name[128];
            snprintf(name, sizeof name, "%s", cp->tok);
            cnext(cp);
            if (t->code == FFI_STRUCT && !t->name) {
                t->name = xstrdup(name);
                scope_add_type(sc, t);
            } else {
                FFIType *alias = type_new(t->code, t->size, t->align);
                alias->name = xstrdup(name);
                alias->array_elem = t->array_elem;
                alias->nelem = t->nelem;
                alias->fields = t->fields;
                alias->n_fields = t->n_fields;
                alias->ffi = t->ffi;
                t->fields = NULL;   /* 所有权移交 alias */
                scope_add_type(sc, alias);
            }
            cexpect(cp, ";");
            continue;
        }
        if (cis(cp, "struct") || cis(cp, "enum") || cis(cp, "union")) {
            bool en = cis(cp, "enum");
            bool un = cis(cp, "union");
            cnext(cp);
            /* 可选名字：struct Foo { ... } / struct Foo; / struct { ... } */
            char sname[128] = {0};
            bool have_name = false;
            if (cp->tkind == 1) {
                const char *save = cp->p;
                snprintf(sname, sizeof sname, "%s", cp->tok);
                cnext(cp);
                if (cis(cp, ";")) {
                    cnext(cp);
                    continue;   /* 前向声明，忽略 */
                }
                if (cis(cp, "{")) {
                    have_name = true;
                } else {
                    /* 不是定义：回退到名字，交给函数/类型路径 */
                    cp->p = save;
                    cnext(cp);
                    sname[0] = 0;
                }
            }
            if (cis(cp, "{") || have_name) {
                /* 构造伪流不现实——直接把名字塞回 tok 再调 parse_struct_body
                 * 不行（p 已过）。改为：手工把 sname 写入临时 token 流不必要——
                 * parse_struct_body 在见到 { 前会再读名字；我们已读。
                 * 方案：回退 p 到名字前，让 parse_struct_body 自己读。 */
                /* 回退：save 在名字前 —— 重新定位。
                 * 简化：have_name 时 p 在 { 上，名字已消费。
                 * 我们把 sname 临时塞进一个合成场景：
                 * 直接内联 body 调用——用 parse_named_body。 */
                FFIType *t = parse_named_body(cp, sc, en, have_name ? sname : NULL);
                if (un && t && t->code == FFI_STRUCT && !t->is_union) {
                    t->is_union = true;
                    size_t maxs = 1, maxa = 1;
                    for (int fi = 0; fi < t->n_fields; fi++) {
                        t->fields[fi].offset = 0;
                        if (t->fields[fi].type->size > maxs) maxs = t->fields[fi].type->size;
                        if (t->fields[fi].type->align > maxa) maxa = t->fields[fi].type->align;
                    }
                    t->align = maxa;
                    t->size = (maxs + maxa - 1) & ~(maxa - 1);
                    if (t->size == 0) t->size = 1;
                    if (t->ffi) {
                        if (t->ffi->elements) tm_free(t->ffi->elements);
                        tm_free(t->ffi);
                        t->ffi = NULL;
                    }
                }
                if (cp->tkind == 1) {
                    /* 可选 typedef 别名：struct Foo { } Bar; */
                    FFIType *alias = type_new(t->code, t->size, t->align);
                    alias->name = xstrdup(cp->tok);
                    alias->array_elem = t->array_elem;
                    alias->nelem = t->nelem;
                    alias->fields = t->fields;
                    alias->n_fields = t->n_fields;
                    alias->ffi = t->ffi;
                    t->fields = NULL;
                    scope_add_type(sc, alias);
                    cnext(cp);
                }
                cexpect(cp, ";");
                continue;
            }
            /* 回退路径：可能是函数返回类型 struct Foo（已回退到名字） */
            if (sname[0]) {
                FFIType *named = NULL;
                for (int i = 0; i < sc->n_types; i++) {
                    if (strcmp(sc->types[i]->name, sname) == 0) {
                        named = sc->types[i];
                        break;
                    }
                }
                if (named) {
                    FFIType *ret = apply_pointers(cp, named);
                    if (cp->tkind == 1) {
                        char fname[128];
                        snprintf(fname, sizeof fname, "%s", cp->tok);
                        cnext(cp);
                        parse_function(cp, sc, ret, fname);
                        continue;
                    }
                }
                tphp_raise("ValueError", "FFI::cdef(): unknown type \"%s\"", sname);
            }
        }
        /* 函数声明 */
        {
            FFIType *base = parse_type(cp, sc);
            if (!base)
                tphp_raise("ValueError", "FFI::cdef(): unexpected token \"%s\"", cp->tok);
            FFIType *ret = apply_pointers(cp, base);
            if (cp->tkind != 1)
                tphp_raise("ValueError", "FFI::cdef(): expected function name");
            char fname[128];
            snprintf(fname, sizeof fname, "%s", cp->tok);
            cnext(cp);
            parse_function(cp, sc, ret, fname);
        }
    }
}

/* 从 { 开始（或先吃可选名字再吃 {）解析 struct/enum body；sname 可为 NULL。
 * 若 sname 非空，当前 token 应是 {（名字已在调用方消费）。 */
static FFIType *parse_named_body(CParser *cp, FFIScope *sc, bool is_enum,
                                 const char *sname) {
    if (!cis(cp, "{"))
        tphp_raise("ValueError", "FFI::cdef(): expected \"{\" in %s definition",
                   is_enum ? "enum" : "struct");
    cnext(cp);

    if (is_enum) {
        FFIType *t = type_new(FFI_ENUM, 4, 4);
        if (sname && sname[0]) t->name = xstrdup(sname);
        int64_t next = 0;
        while (!cis(cp, "}")) {
            if (cp->tkind != 1)
                tphp_raise("ValueError", "FFI::cdef(): expected enumerator name");
            char ename[128];
            snprintf(ename, sizeof ename, "%s", cp->tok);
            cnext(cp);
            int64_t val = next;
            if (cis(cp, "=")) {
                cnext(cp);
                bool neg = false;
                if (cis(cp, "-")) { neg = true; cnext(cp); }
                if (cp->tkind != 2)
                    tphp_raise("ValueError", "FFI::cdef(): expected integer after '='");
                val = strtoll(cp->tok, NULL, 0);
                if (neg) val = -val;
                cnext(cp);
            }
            next = val + 1;
            scope_add_const(sc, ename, val);
            if (cis(cp, ",")) cnext(cp);
            else break;
        }
        cexpect(cp, "}");
        if (t->name) scope_add_type(sc, t);
        return t;
    }

    FFIType *t = type_new(FFI_STRUCT, 0, 1);
    if (sname && sname[0]) {
        t->name = xstrdup(sname);
        scope_add_type(sc, t);
    }

    int cap = 8;
    t->fields = tm_malloc(sizeof(*t->fields) * (size_t)cap);
    while (!cis(cp, "}")) {
        while (cis(cp, ";")) cnext(cp);
        if (cis(cp, "}")) break;
        if (cis(cp, "struct") || cis(cp, "enum") || cis(cp, "union")) {
            bool en = cis(cp, "enum");
            bool un = cis(cp, "union");
            (void)un;   /* 内嵌 union 字段：解析后 is_union 在 emb 上 */
            cnext(cp);
            char nbuf[128] = {0};
            bool named = false;
            if (cp->tkind == 1 && !cis(cp, "{")) {
                snprintf(nbuf, sizeof nbuf, "%s", cp->tok);
                cnext(cp);
                named = true;
            }
            FFIType *emb;
            if (named && cis(cp, ";")) {
                /* 内嵌前向：查已有 */
                emb = NULL;
                for (int i = 0; i < sc->n_types; i++)
                    if (strcmp(sc->types[i]->name, nbuf) == 0) { emb = sc->types[i]; break; }
                if (!emb)
                    tphp_raise("ValueError", "FFI::cdef(): unknown type \"%s\"", nbuf);
                cnext(cp);
            } else if (named && !cis(cp, "{")) {
                /* 已命名类型引用：struct Foo field; / struct Foo *ptr; / union U u; */
                emb = NULL;
                for (int i = 0; i < sc->n_types; i++)
                    if (sc->types[i]->name && strcmp(sc->types[i]->name, nbuf) == 0) {
                        emb = sc->types[i]; break;
                    }
                if (!emb)
                    tphp_raise("ValueError", "FFI::cdef(): unknown type \"%s\"", nbuf);
                /* 名字已消费；支持 `struct Foo *ptr` / `**pp`（当前在 * 或字段名上） */
                emb = apply_pointers(cp, emb);
            } else {
                emb = parse_named_body(cp, sc, en, named ? nbuf : NULL);
                /* 内嵌 union：重算布局 */
                if (un && emb && emb->code == FFI_STRUCT && !emb->is_union) {
                    emb->is_union = true;
                    size_t maxs = 1, maxa = 1;
                    for (int fi = 0; fi < emb->n_fields; fi++) {
                        emb->fields[fi].offset = 0;
                        if (emb->fields[fi].type->size > maxs) maxs = emb->fields[fi].type->size;
                        if (emb->fields[fi].type->align > maxa) maxa = emb->fields[fi].type->align;
                    }
                    emb->align = maxa;
                    emb->size = (maxs + maxa - 1) & ~(maxa - 1);
                    if (emb->size == 0) emb->size = 1;
                    if (emb->ffi) {
                        if (emb->ffi->elements) tm_free(emb->ffi->elements);
                        tm_free(emb->ffi);
                        emb->ffi = NULL;
                    }
                }
            }
            char fname[128] = {0};
            if (cp->tkind == 1) {
                snprintf(fname, sizeof fname, "%s", cp->tok);
                cnext(cp);
            }
            if (cis(cp, "[")) {
                cnext(cp);
                if (cp->tkind != 2)
                    tphp_raise("ValueError", "FFI::cdef(): expected array size");
                int64_t n = strtoll(cp->tok, NULL, 0);
                cnext(cp);
                cexpect(cp, "]");
                FFIType *at = type_new(FFI_ARRAY, emb->size * (size_t)n, emb->align);
                at->array_elem = emb;
                at->nelem = (size_t)n;
                emb = at;
            }
            if (t->n_fields >= cap) {
                cap *= 2;
                t->fields = tm_realloc(t->fields, sizeof(*t->fields) * (size_t)cap);
            }
            t->fields[t->n_fields].name = xstrdup(fname);
            t->fields[t->n_fields].type = emb;
            t->fields[t->n_fields].offset = 0;
            t->n_fields++;
            cexpect(cp, ";");
            continue;
        }
        FFIType *base = parse_type(cp, sc);
        if (!base)
            tphp_raise("ValueError", "FFI::cdef(): bad field type near \"%s\"", cp->tok);
        for (;;) {
            char fname[128] = {0};
            FFIType *ft = parse_declarator(cp, sc, base, fname, sizeof fname, false);
            if (t->n_fields >= cap) {
                cap *= 2;
                t->fields = tm_realloc(t->fields, sizeof(*t->fields) * (size_t)cap);
            }
            t->fields[t->n_fields].name = xstrdup(fname);
            t->fields[t->n_fields].type = ft;
            t->fields[t->n_fields].offset = 0;
            t->n_fields++;
            if (cis(cp, ",")) { cnext(cp); continue; }
            break;
        }
        cexpect(cp, ";");
    }
    cexpect(cp, "}");

    if (t->is_union) {
        /* union：全部字段 offset=0，size=max，align=max */
        size_t maxs = 1, maxa = 1;
        for (int i = 0; i < t->n_fields; i++) {
            t->fields[i].offset = 0;
            if (t->fields[i].type->size > maxs) maxs = t->fields[i].type->size;
            if (t->fields[i].type->align > maxa) maxa = t->fields[i].type->align;
        }
        t->align = maxa;
        t->size = (maxs + maxa - 1) & ~(maxa - 1);
        if (t->size == 0) t->size = 1;
    } else {
        size_t off = 0, maxa = 1;
        for (int i = 0; i < t->n_fields; i++) {
            FFIType *ft = t->fields[i].type;
            size_t a = ft->align;
            if (a > maxa) maxa = a;
            off = (off + a - 1) & ~(a - 1);
            t->fields[i].offset = off;
            off += ft->size;
        }
        t->align = maxa;
        t->size = (off + maxa - 1) & ~(maxa - 1);
        if (t->size == 0) t->size = 1;
    }
    return t;
}

/* 打包 DLL 解析：清单命中的相对路径 → 指纹缓存内等效绝对路径（目录结构
 * 与包内一致，依赖链可同级解析）。未打包/系统库名 → 原样加载。
 * Windows 一律宽字符（UTF-8 输入）；命中的绝对路径加
 * LOAD_WITH_ALTERED_SEARCH_PATH 让依赖优先在 DLL 自身目录查找。 */
static void *ffi_lib_open(const char *libname) {
    const char *root = tphp_pack_dll_root();
    char joined[2048];
    const char *use = libname;
    bool resolved = false;
    if (root && libname[0] && tphp_pack_stat(libname, NULL)) {
        snprintf(joined, sizeof joined, "%s/%s", root, libname);
        use = joined;
        resolved = true;
    }
#ifdef _WIN32
    wchar_t wu[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, use, -1, wu, 1024)) return NULL;
    if (resolved)
        return LoadLibraryExW(wu, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    return LoadLibraryW(wu);
#else
    return dlopen(use, RTLD_LAZY | RTLD_LOCAL);
#endif
}

static FFIScope *scope_new(const char *code, const char *libname) {
    check_no_preproc(code);
    FFIScope *sc = tm_malloc(sizeof *sc);
    memset(sc, 0, sizeof *sc);
    if (libname && libname[0]) {
        sc->lib = ffi_lib_open(libname);
        if (!sc->lib)
            tphp_raise("ValueError", "FFI::cdef(): failed to load library \"%s\"", libname);
    }
    CParser cp = { .s = code, .p = code, .end = code + strlen(code) };
    cnext(&cp);
    parse_toplevel(&cp, sc);
    return sc;
}

static FFIFunc *scope_find_func(FFIScope *sc, const char *name) {
    for (int i = 0; i < sc->n_funcs; i++)
        if (strcmp(sc->funcs[i]->name, name) == 0)
            return sc->funcs[i];
    return NULL;
}

/* ---- CData 包装 ---- */

static TVal cdata_wrap(void *ptr, FFIType *t, bool owned, FFIScope *sc) {
    FFCData *d = tm_malloc(sizeof *d);
    d->ptr = ptr;
    d->type = t;
    d->owned = owned;
    d->scope = sc;
    int idx = cdata_reg(d);
    TObj *o = tphp_obj_alloc(g_cdata_cls);
    o->props[0] = TV_INT(idx);
    return TV_OBJ(o);
}

TVal tphp_ffi_wrap_ptr(void *ptr, void *type, bool owned, void *scope) {
    return cdata_wrap(ptr, (FFIType *)type, owned, (FFIScope *)scope);
}

static void cdata_dtor(TObj *o) {
    if (o->props[0].t != T_INT) return;
    int64_t i = o->props[0].u.i;
    if (i < 0 || i >= g_n_cdatas) return;
    FFCData *d = g_cdatas[i];
    if (d && d->owned && d->ptr) {
        tm_free(d->ptr);
        d->ptr = NULL;
        d->owned = false;
    }
}

static FFCData *cdata_of(VM *vm, TVal v, const char *who) {
    if (v.t != T_OBJ)
        tphp_vm_raise(vm, "TypeError", "%s() expects an FFI\\CData object", who);
    TObj *o = (TObj *)v.u.p;
    if (o->cls != g_cdata_cls || o->props[0].t != T_INT)
        tphp_vm_raise(vm, "TypeError", "%s() expects an FFI\\CData object", who);
    int64_t i = o->props[0].u.i;
    if (i < 0 || i >= g_n_cdatas || !g_cdatas[i] || !g_cdatas[i]->ptr)
        tphp_vm_raise(vm, "Error", "%s(): CData pointer is null or freed", who);
    return g_cdatas[i];
}

/* ---- 值编解码 ---- */

/* UTF-8 → wide（Windows UTF-16 / POSIX UTF-32）；返回 malloc 缓冲，*out_nels
 * 为元素个数（不含 NUL）。调用方 tm_free。 */
static void *utf8_to_wide(const char *s, size_t slen, size_t *out_nels) {
#ifdef _WIN32
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, (int)slen, NULL, 0);
    if (wn <= 0) { *out_nels = 0; return NULL; }
    wchar_t *w = tm_malloc((size_t)(wn + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, (int)slen, w, wn);
    w[wn] = 0;
    *out_nels = (size_t)wn;
    return w;
#else
    /* 简化：Latin-1 按字节提升（完整 UTF-32 转换后置） */
    uint32_t *w = tm_malloc((slen + 1) * sizeof(uint32_t));
    for (size_t i = 0; i < slen; i++) w[i] = (uint32_t)(unsigned char)s[i];
    w[slen] = 0;
    *out_nels = slen;
    return w;
#endif
}

/* wide → UTF-8；返回 TStr*（+1） */
static TStr *wide_to_utf8(const void *w, size_t max_els) {
#ifdef _WIN32
    const wchar_t *wp = w;
    size_t n = 0;
    while (n < max_els && wp[n]) n++;
    if (n == 0) return tstr_new("", 0);
    int bn = WideCharToMultiByte(CP_UTF8, 0, wp, (int)n, NULL, 0, NULL, NULL);
    if (bn <= 0) return tstr_new("", 0);
    TStr *s = tstr_alloc_raw((uint32_t)bn, false);
    WideCharToMultiByte(CP_UTF8, 0, wp, (int)n, s->c, bn, NULL, NULL);
    s->c[bn] = 0;
    s->len = (uint32_t)bn;
    gc_register(&s->h);
    return s;
#else
    const uint32_t *wp = w;
    size_t n = 0;
    while (n < max_els && wp[n]) n++;
    TStr *s = tstr_alloc_raw((uint32_t)n, false);
    for (size_t i = 0; i < n; i++) s->c[i] = (char)(wp[i] < 128 ? wp[i] : '?');
    s->c[n] = 0;
    s->len = (uint32_t)n;
    gc_register(&s->h);
    return s;
#endif
}

/* 编码调用期间暂存的 wide 转换缓冲（call 结束后统一 free） */
#define FFI_MAX_WBUF 8
static void *g_wbuf_hold[FFI_MAX_WBUF];
static int g_n_wbuf;

static void wbuf_reset(void) { g_n_wbuf = 0; }
static void wbuf_track(void *p) {
    if (g_n_wbuf < FFI_MAX_WBUF) g_wbuf_hold[g_n_wbuf++] = p;
}
static void wbuf_free_all(void) {
    for (int i = 0; i < g_n_wbuf; i++) tm_free(g_wbuf_hold[i]);
    g_n_wbuf = 0;
}

/* ============================================================
 * 回调 trampoline（PLAN/02 §4.5.1）：闭包 → C 函数指针。
 * 生命周期：trampoline + 闭包（addref）进线程局部表，脚本/线程结束释放
 * （CLI 单脚本 ≈ PHP 单请求"整请求有效"）。
 * 异常（v2-3）：tphp_vm_call_bounded 的 local setjmp 接管 raise——
 * 回调内 try/catch 正常工作；未捕获 = E_WARNING 风格 warning + 零值返回，
 * 进程不死（对齐 PHP；longjmp 不穿 ffi_call 的 C 栈）。
 * ============================================================ */

typedef struct FFIClosure {
    ffi_closure *cl;
    void *exec;              /* 可执行地址（传给 C 的值） */
    ffi_cif cif;             /* 回调签名 */
    ffi_type **cif_atypes;
    TVal fn;                 /* PHP 闭包（已 addref） */
    FFIType *sig;            /* FFI_FUNC_PTR（借用 scope 类型，活到线程结束） */
} FFIClosure;

static _Thread_local FFIClosure **g_closures;
static _Thread_local int g_n_closures, g_cap_closures;

/* vm.c vm_raise_at 曾查询：>0 时 Fatal（v1）；v2-3 起由 trampoline 的
 * local setjmp 接管，本计数仅保留嵌套深度语义（调试/未来 trace 用） */
_Thread_local int tphp_ffi_cb_depth = 0;

/* 宽松标量转换（回调返回路径；不 raise——非数值当 0） */
static int64_t loose_int(TVal v) {
    if (v.t == T_INT) return v.u.i;
    if (v.t == T_FLOAT) return (int64_t)v.u.d;
    if (v.t == T_TRUE) return 1;
    return 0;
}
static double loose_float(TVal v) {
    if (v.t == T_FLOAT) return v.u.d;
    if (v.t == T_INT) return (double)v.u.i;
    return 0.0;
}

/* C → PHP 参数解码（libffi trampoline args[i]：标量/指针指向值槽，
 * struct 指向栈上数据——struct 视图仅回调期间有效，与 PHP 同款约定） */
static TVal cb_decode_arg(FFIType *t, void *p) {
    switch (t->code) {
    case FFI_BOOL:  return *(uint8_t *)p ? TV_TRUE : TV_FALSE;
    case FFI_INT8:  return TV_INT(*(int8_t *)p);
    case FFI_UINT8: return TV_INT(*(uint8_t *)p);
    case FFI_INT16: return TV_INT(*(int16_t *)p);
    case FFI_UINT16:return TV_INT(*(uint16_t *)p);
    case FFI_INT32: return TV_INT(*(int32_t *)p);
    case FFI_UINT32:return TV_INT(*(uint32_t *)p);
    case FFI_INT64: return TV_INT(*(int64_t *)p);
    case FFI_UINT64:return TV_INT((int64_t)*(uint64_t *)p);
    case FFI_FLOAT: return TV_FLOAT(*(float *)p);
    case FFI_DOUBLE:return TV_FLOAT(*(double *)p);
    case FFI_ENUM:  return TV_INT(*(int32_t *)p);
    case FFI_WCHAR:
#ifdef _WIN32
        return TV_INT(*(uint16_t *)p);
#else
        return TV_INT(*(uint32_t *)p);
#endif
    case FFI_PTR: {
        void *q = *(void **)p;
        if (!q) return TV_NULL;
        /* char / int8 指针 → string 拷贝（PHP const char* 语义；tphp 无
         * const 标记，统一拷贝防悬空——与 PHP 的 char*=CData 视图有意差异） */
        if (t->array_elem && t->array_elem->code == FFI_INT8) {
            size_t ln = 0;
            while (((char *)q)[ln]) ln++;
            return TV_STR(tstr_new((char *)q, ln));
        }
        return cdata_wrap(q, t->array_elem ? t->array_elem : bt_void,
                          false, NULL);
    }
    case FFI_STRUCT:
        /* 别名 libffi 栈参数：回调返回后失效（PHP 同款；文档红字） */
        return cdata_wrap(p, t, false, NULL);
    case FFI_ARRAY:
        return cdata_wrap(p, t, false, NULL);
    case FFI_FUNC_PTR: {
        void *q = *(void **)p;
        return q ? cdata_wrap(q, t, false, NULL) : TV_NULL;
    }
    case FFI_VOID: break;
    }
    return TV_NULL;
}

/* PHP → C 返回值（类型不匹配 → stderr 警告 + 零值；修 PHP 的"吞异常
 * 返回未初始化垃圾"坑）。r 的所有权仍归调用方。 */
static void cb_encode_ret(FFIType *t, void *ret, TVal r) {
    if (t->code == FFI_VOID) return;
    memset(ret, 0, t->size ? t->size : 1);
    switch (t->code) {
    case FFI_BOOL:  *(uint8_t *)ret = loose_int(r) ? 1 : 0; return;
    case FFI_INT8:  *(int8_t *)ret = (int8_t)loose_int(r); return;
    case FFI_UINT8: *(uint8_t *)ret = (uint8_t)loose_int(r); return;
    case FFI_INT16: *(int16_t *)ret = (int16_t)loose_int(r); return;
    case FFI_UINT16:*(uint16_t *)ret = (uint16_t)loose_int(r); return;
    case FFI_INT32: *(int32_t *)ret = (int32_t)loose_int(r); return;
    case FFI_UINT32:*(uint32_t *)ret = (uint32_t)loose_int(r); return;
    case FFI_INT64: *(int64_t *)ret = loose_int(r); return;
    case FFI_UINT64:*(uint64_t *)ret = (uint64_t)loose_int(r); return;
    case FFI_FLOAT: *(float *)ret = (float)loose_float(r); return;
    case FFI_DOUBLE:*(double *)ret = loose_float(r); return;
    case FFI_ENUM:  *(int32_t *)ret = (int32_t)loose_int(r); return;
    case FFI_WCHAR:
#ifdef _WIN32
        *(uint16_t *)ret = (uint16_t)loose_int(r);
#else
        *(uint32_t *)ret = (uint32_t)loose_int(r);
#endif
        return;
    case FFI_PTR:
        if (r.t == T_NULL) { *(void **)ret = NULL; return; }
        if (r.t == T_OBJ) {
            TObj *o = (TObj *)r.u.p;
            if (o->cls == g_cdata_cls && o->props[0].t == T_INT) {
                *(void **)ret = g_cdatas[o->props[0].u.i]->ptr;
                return;
            }
        }
        if (r.t == T_STR) { *(void **)ret = ((TStr *)r.u.p)->c; return; }
        fprintf(stderr, "PHP Warning: FFI callback return: "
                        "expected pointer-compatible value\n");
        return;
    case FFI_STRUCT:
        if (r.t == T_OBJ) {
            TObj *o = (TObj *)r.u.p;
            if (o->cls == g_cdata_cls && o->props[0].t == T_INT) {
                FFCData *d = g_cdatas[o->props[0].u.i];
                if (d->type->size == t->size) {
                    memcpy(ret, d->ptr, t->size);
                    return;
                }
            }
        }
        fprintf(stderr, "PHP Warning: FFI callback return: "
                        "struct return requires a same-size CData\n");
        return;
    default:
        fprintf(stderr, "PHP Warning: FFI callback return: "
                        "unsupported return type\n");
        return;
    }
}

#define CB_MAX_ARGS 16

/* vm.c（非公共头）：以 callable 调用一帧——回调 trampoline 用 */
extern TVal tphp_vm_call_callable(struct VM *vm, TVal callable,
                                  TVal *args, int n);
/* v2-3：有界回调调用（setjmp + floor unwind；见 PLAN/02 §4.5.1） */
extern bool tphp_vm_call_bounded(struct VM *vm, TVal callable,
                                 TVal *args, int n, TVal *out);

static void cb_trampoline(ffi_cif *cif, void *ret, void **args,
                          void *user_data) {
    (void)cif;
    FFIClosure *fc = (FFIClosure *)user_data;
    FFIType *sig = fc->sig;
    VM *vm = (VM *)tphp_vm_current;
    tphp_ffi_cb_depth++;
    int n = sig->fn_n > CB_MAX_ARGS ? CB_MAX_ARGS : sig->fn_n;
    TVal argv[CB_MAX_ARGS];
    for (int i = 0; i < n; i++)
        argv[i] = cb_decode_arg(sig->fn_params[i], args[i]);
    TVal r = TV_NULL;
    if (vm) {
        if (!tphp_vm_call_bounded(vm, fc->fn, argv, n, &r)) {
            /* 逃逸：对齐 PHP E_WARNING——未捕获异常 warning + 零值返回，
             * 进程不死；回调内 try/catch 已在 bounded 内处理 */
            TExObj *ex = vm->x.exception;
            if (ex) {
                const char *cls = (ex->wrap && ex->wrap->cls->name)
                    ? tstr_val(ex->wrap->cls->name) : ex->cls->name;
                const char *file = ex->file ? ex->file : "-";
                fprintf(stderr,
                        "PHP Warning: Uncaught %s: %s in %s:%d in FFI callback\n"
                        "  thrown in %s on line %d\n",
                        cls, tstr_val(ex->message), file, ex->line,
                        file, ex->line);
                TVal xv = TV_OBJ(ex);
                vm->x.exception = NULL;
                tval_release(&xv);
            }
            r = TV_NULL;              /* 零值返回（cb_encode_ret 宽松语义） */
        }
    } else {
        fprintf(stderr, "PHP Warning: FFI callback invoked without a VM\n");
    }
    for (int i = 0; i < n; i++) tval_release(&argv[i]);
    cb_encode_ret(sig->fn_ret, ret, r);
    tval_release(&r);
    tphp_ffi_cb_depth--;
}

/* 闭包 → 可执行 trampoline 地址；失败 raise。
 * v2②：同一闭包（TVal 指针）+ 同一签名（FFIType*）复用同一 trampoline
 * ——PHP 每传建一、泄漏到请求末；tphp 脚本级保活下循环传闭包会线性堆
 * 积 libffi closure，去重后平稳。复用键：fn 对象指针 + sig 指针（同一
 * cdef 作用域的同名声明恒为同一 FFIType*）。非闭包 callable（字符串等）
 * 不去重（v1 范围外）。 */
static void *closure_create(VM *vm, FFIType *sig, TVal fn,
                            const char *fname, int idx) {
    if (sig->fn_n > CB_MAX_ARGS)
        tphp_vm_raise(vm, "Error",
                      "%s(): Argument #%d callback has too many parameters "
                      "(max %d)", fname, idx + 1, CB_MAX_ARGS);
    if (fn.t == T_CLOSURE) {
        for (int i = 0; i < g_n_closures; i++) {
            if (g_closures[i]->fn.t == T_CLOSURE &&
                g_closures[i]->fn.u.p == fn.u.p &&
                g_closures[i]->sig == sig)
                return g_closures[i]->exec;
        }
    }
    FFIClosure *fc = tm_malloc(sizeof *fc);
    memset(fc, 0, sizeof *fc);
    fc->sig = sig;
    fc->fn = fn;
    tval_addref(fn);
    ffi_type **ats = tm_malloc(sizeof(ffi_type *) *
                               (size_t)(sig->fn_n > 0 ? sig->fn_n : 1));
    fc->cif_atypes = ats;
    for (int i = 0; i < sig->fn_n; i++) {
        if (sig->fn_params[i]->code == FFI_STRUCT)
            prepare_struct_ffi(sig->fn_params[i]);
        ats[i] = to_ffi_type(sig->fn_params[i]);
    }
    if (sig->fn_ret->code == FFI_STRUCT) prepare_struct_ffi(sig->fn_ret);
    if (ffi_prep_cif(&fc->cif, FFI_DEFAULT_ABI, (unsigned)sig->fn_n,
                     to_ffi_type(sig->fn_ret), ats) != FFI_OK) {
        /* raise 前收尾（longjmp 不会回来——不学 PHP 的静默泄漏） */
        tval_release(&fc->fn);
        tm_free(ats);
        tm_free(fc);
        tphp_vm_raise(vm, "Error",
                      "%s(): Argument #%d ffi_prep_cif failed for callback",
                      fname, idx + 1);
    }
    void *code = NULL;
    fc->cl = ffi_closure_alloc(sizeof(ffi_closure), &code);
    if (!fc->cl || !code) {
        tval_release(&fc->fn);
        tm_free(ats);
        tm_free(fc);
        tphp_vm_raise(vm, "Error",
                      "%s(): Argument #%d ffi_closure_alloc failed",
                      fname, idx + 1);
    }
    fc->exec = code;
    if (ffi_prep_closure_loc(fc->cl, &fc->cif, cb_trampoline, fc, code)
        != FFI_OK) {
        ffi_closure_free(fc->cl);
        tval_release(&fc->fn);
        tm_free(ats);
        tm_free(fc);
        tphp_vm_raise(vm, "Error",
                      "%s(): Argument #%d ffi_prep_closure_loc failed",
                      fname, idx + 1);
    }
    /* 线程局部表保活（脚本/线程结束释放） */
    if (g_n_closures >= g_cap_closures) {
        g_cap_closures = g_cap_closures ? g_cap_closures * 2 : 8;
        g_closures = tm_realloc(g_closures,
                                sizeof(FFIClosure *) * (size_t)g_cap_closures);
    }
    g_closures[g_n_closures++] = fc;
    return code;
}

static void encode_arg(VM *vm, FFIType *t, TVal v, void *slot,
                       const char *fname, int idx) {
    switch (t->code) {
    case FFI_VOID: break;
    case FFI_BOOL: *(uint8_t *)slot = tval_to_bool(v) ? 1 : 0; break;
    case FFI_INT8:  *(int8_t *)slot = (int8_t)tval_to_int(v); break;
    case FFI_UINT8: *(uint8_t *)slot = (uint8_t)tval_to_int(v); break;
    case FFI_INT16: *(int16_t *)slot = (int16_t)tval_to_int(v); break;
    case FFI_UINT16:*(uint16_t *)slot = (uint16_t)tval_to_int(v); break;
    case FFI_INT32: *(int32_t *)slot = (int32_t)tval_to_int(v); break;
    case FFI_UINT32:*(uint32_t *)slot = (uint32_t)tval_to_int(v); break;
    case FFI_INT64: *(int64_t *)slot = tval_to_int(v); break;
    case FFI_UINT64:*(uint64_t *)slot = (uint64_t)tval_to_int(v); break;
    case FFI_FLOAT: *(float *)slot = (float)tval_to_float(v); break;
    case FFI_DOUBLE:*(double *)slot = tval_to_float(v); break;
    case FFI_ENUM:  *(int32_t *)slot = (int32_t)tval_to_int(v); break;
    case FFI_WCHAR: {
        /* wchar 标量：按 int 写入 */
        if (v.t == T_STR) {
            TStr *s = (TStr *)v.u.p;
            uint32_t cp = (unsigned char)s->c[0];
            if (s->len > 0) {
#ifdef _WIN32
                *(uint16_t *)slot = (uint16_t)cp;
#else
                *(uint32_t *)slot = cp;
#endif
            }
            break;
        }
#ifdef _WIN32
        *(uint16_t *)slot = (uint16_t)tval_to_int(v);
#else
        *(uint32_t *)slot = (uint32_t)tval_to_int(v);
#endif
        break;
    }
    case FFI_PTR: {
        if (v.t == T_NULL) { *(void **)slot = NULL; break; }
        if (v.t == T_STR) {
            TStr *s = (TStr *)v.u.p;
            /* wchar* 参数：UTF-8 → wide 临时缓冲 */
            if (t->array_elem && t->array_elem->code == FFI_WCHAR) {
                size_t nels = 0;
                void *w = utf8_to_wide(s->c, s->len, &nels);
                wbuf_track(w);
                *(void **)slot = w;
                break;
            }
            *(void **)slot = (void *)s->c;
            break;
        }
        if (v.t == T_INT) { *(void **)slot = (void *)(intptr_t)v.u.i; break; }
        if (v.t == T_OBJ) {
            TObj *o = (TObj *)v.u.p;
            if (o->cls == g_cdata_cls && o->props[0].t == T_INT) {
                *(void **)slot = g_cdatas[o->props[0].u.i]->ptr;
                break;
            }
        }
        tphp_vm_raise(vm, "TypeError",
                      "%s(): Argument #%d expects pointer-compatible value",
                      fname, idx + 1);
        break;
    }
    case FFI_FUNC_PTR: {
        if (v.t == T_NULL) { *(void **)slot = NULL; break; }
        if (v.t == T_CLOSURE) {
            *(void **)slot = closure_create(vm, t, v, fname, idx);
            break;
        }
        /* 已有函数指针 CData 透传（如回调参数里收到的指针再传下去） */
        if (v.t == T_OBJ) {
            TObj *o = (TObj *)v.u.p;
            if (o->cls == g_cdata_cls && o->props[0].t == T_INT) {
                *(void **)slot = g_cdatas[o->props[0].u.i]->ptr;
                break;
            }
        }
        tphp_vm_raise(vm, "TypeError",
                      "%s(): Argument #%d expects a closure or null for "
                      "function pointer type", fname, idx + 1);
        break;
    }
    case FFI_STRUCT: {
        if (v.t == T_OBJ) {
            TObj *o = (TObj *)v.u.p;
            if (o->cls == g_cdata_cls && o->props[0].t == T_INT) {
                FFCData *d = g_cdatas[o->props[0].u.i];
                if (d->type->size != t->size)
                    tphp_vm_raise(vm, "TypeError",
                                  "%s(): Argument #%d struct size mismatch",
                                  fname, idx + 1);
                memcpy(slot, d->ptr, t->size);
                break;
            }
        }
        tphp_vm_raise(vm, "TypeError",
                      "%s(): Argument #%d expects CData for struct type",
                      fname, idx + 1);
        break;
    }
    case FFI_ARRAY:
        tphp_vm_raise(vm, "TypeError",
                      "%s(): Argument #%d cannot pass array by value",
                      fname, idx + 1);
        break;
    }
}

/* ---- libffi 调用 ---- */

#define FFI_MAX_ARGS 16

typedef union {
    int64_t i64; uint64_t u64;
    double d; float f;
    void *p;
    uint8_t raw[32];
} FFIArgSlot;

static TVal ffi_do_call(VM *vm, FFIFunc *fn, TVal *args, int n) {
    if (!fn->fn)
        fn->fn = resolve_symbol((FFIScope *)fn->sc, fn->name);
    if (!fn->fn)
        tphp_vm_raise(vm, "Error", "FFI::%s(): symbol not found", fn->name);
    if (!fn->variadic && n != fn->n_params)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "FFI::%s() expects %d argument%s, %d given",
                      fn->name, fn->n_params,
                      fn->n_params == 1 ? "" : "s", n);
    if (fn->variadic && n < fn->n_params)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "FFI::%s() expects at least %d arguments, %d given",
                      fn->name, fn->n_params, n);
    if (n > FFI_MAX_ARGS)
        tphp_vm_raise(vm, "Error", "FFI::%s(): too many arguments", fn->name);

    void *aval[FFI_MAX_ARGS];
    FFIArgSlot slots[FFI_MAX_ARGS];
    void *heap[FFI_MAX_ARGS];
    int nheap = 0;
    wbuf_reset();

    for (int i = 0; i < n; i++) {
        FFIType *pt;
        bool is_extra = i >= fn->n_params;
        if (!is_extra) {
            pt = fn->params[i];
        } else {
            TVal v = args[i];
            if (v.t == T_FLOAT) { slots[i].d = v.u.d; aval[i] = &slots[i]; continue; }
            if (v.t == T_STR) {
                slots[i].p = (void *)((TStr *)v.u.p)->c;
                aval[i] = &slots[i];
                continue;
            }
            slots[i].i64 = tval_to_int(v);
            aval[i] = &slots[i];
            continue;
        }
        if (pt->code == FFI_STRUCT) {
            void *buf = tm_malloc(pt->size ? pt->size : 1);
            heap[nheap++] = buf;
            encode_arg(vm, pt, args[i], buf, fn->name, i);
            aval[i] = buf;
        } else {
            memset(&slots[i], 0, sizeof slots[i]);
            encode_arg(vm, pt, args[i], &slots[i], fn->name, i);
            aval[i] = &slots[i];
        }
    }

    uint8_t stack_r[64];
    void *heap_r = NULL;
    void *rbuf;
    if (fn->ret->code == FFI_STRUCT && fn->ret->size > sizeof stack_r) {
        heap_r = tm_malloc(fn->ret->size);
        rbuf = heap_r;
    } else {
        memset(stack_r, 0, sizeof stack_r);
        rbuf = stack_r;
    }

    if (fn->variadic && n > fn->n_params) {
        ffi_type *atypes[FFI_MAX_ARGS];
        for (int i = 0; i < n; i++) {
            if (i < fn->n_params) {
                if (fn->params[i]->code == FFI_STRUCT) prepare_struct_ffi(fn->params[i]);
                atypes[i] = to_ffi_type(fn->params[i]);
            } else if (args[i].t == T_FLOAT) {
                atypes[i] = &ffi_type_double;
            } else if (args[i].t == T_STR) {
                atypes[i] = &ffi_type_pointer;
            } else {
                atypes[i] = &ffi_type_sint64;
            }
        }
        ffi_cif vif;
        if (ffi_prep_cif_var(&vif, FFI_DEFAULT_ABI, (unsigned)fn->n_params,
                             (unsigned)n, to_ffi_type(fn->ret), atypes) != FFI_OK) {
            for (int k = 0; k < nheap; k++) tm_free(heap[k]);
            if (heap_r) tm_free(heap_r);
            wbuf_free_all();
            tphp_vm_raise(vm, "Error", "FFI::%s(): ffi_prep_cif_var failed", fn->name);
        }
        ffi_call(&vif, FFI_FN(fn->fn), rbuf, aval);
    } else {
        ffi_call(&fn->cif, FFI_FN(fn->fn), rbuf, aval);
    }

    for (int k = 0; k < nheap; k++) tm_free(heap[k]);
    wbuf_free_all();

    TVal out;
    switch (fn->ret->code) {
    case FFI_VOID:  out = TV_NULL; break;
    case FFI_BOOL:  out = *(uint8_t *)rbuf ? TV_TRUE : TV_FALSE; break;
    case FFI_INT8:  out = TV_INT(*(int8_t *)rbuf); break;
    case FFI_UINT8: out = TV_INT(*(uint8_t *)rbuf); break;
    case FFI_INT16: out = TV_INT(*(int16_t *)rbuf); break;
    case FFI_UINT16:out = TV_INT(*(uint16_t *)rbuf); break;
    case FFI_INT32: out = TV_INT(*(int32_t *)rbuf); break;
    case FFI_UINT32:out = TV_INT(*(uint32_t *)rbuf); break;
    case FFI_INT64: out = TV_INT(*(int64_t *)rbuf); break;
    case FFI_UINT64:out = TV_INT((int64_t)*(uint64_t *)rbuf); break;
    case FFI_FLOAT: out = TV_FLOAT(*(float *)rbuf); break;
    case FFI_DOUBLE:out = TV_FLOAT(*(double *)rbuf); break;
    case FFI_ENUM:  out = TV_INT(*(int32_t *)rbuf); break;
    case FFI_WCHAR:
#ifdef _WIN32
        out = TV_INT(*(uint16_t *)rbuf);
#else
        out = TV_INT(*(uint32_t *)rbuf);
#endif
        break;
    case FFI_PTR: {
        void *p = *(void **)rbuf;
        if (!p) { out = TV_NULL; break; }
        if (fn->ret->array_elem && fn->ret->array_elem->code == FFI_INT8) {
            size_t ln = 0;
            while (((char *)p)[ln]) ln++;
            out = TV_STR(tstr_new((char *)p, ln));
        } else if (fn->ret->array_elem && fn->ret->array_elem->code == FFI_WCHAR) {
            out = TV_STR(wide_to_utf8(p, 4096));
        } else {
            out = cdata_wrap(p, fn->ret->array_elem ? fn->ret->array_elem : bt_void,
                             false, (FFIScope *)fn->sc);
        }
        break;
    }
    case FFI_STRUCT: {
        void *buf = heap_r ? heap_r : tm_malloc(fn->ret->size ? fn->ret->size : 1);
        if (!heap_r) memcpy(buf, rbuf, fn->ret->size);
        out = cdata_wrap(buf, fn->ret, true, (FFIScope *)fn->sc);
        heap_r = NULL;
        break;
    }
    default:
        out = TV_NULL;
    }
    if (heap_r) tm_free(heap_r);
    return out;
}

/* ---- 类型字符串解析（FFI::new 等） ---- */

static FFIType *parse_type_string(VM *vm, const char *s, FFIScope *sc) {
    builtin_types_init();
    CParser cp = { .s = s, .p = s, .end = s + strlen(s) };
    cnext(&cp);
    FFIType *base = parse_type(&cp, sc);
    if (!base)
        tphp_vm_raise(vm, "ValueError", "cannot parse type \"%s\"", s);
    /* 类型表达式：[*]* [name] [N] —— 名字可省略（"int" / "void*" / "int[4]"） */
    FFIType *t = apply_pointers(&cp, base);
    if (cp.tkind == 1) cnext(&cp);          /* 可选名字，丢弃 */
    while (cis(&cp, "[")) {
        cnext(&cp);
        size_t n = 1;
        if (cp.tkind == 2) {
            n = (size_t)strtoull(cp.tok, NULL, 0);
            cnext(&cp);
        }
        cexpect(&cp, "]");
        FFIType *at = type_new(FFI_ARRAY, t->size * n, t->align);
        at->array_elem = t;
        at->nelem = n;
        t = at;
    }
    if (cp.tkind != 0)
        tphp_vm_raise(vm, "ValueError", "trailing tokens in type \"%s\"", s);
    return t;
}

/* ---- native 方法 ---- */

static TVal nm_ffi_cdef(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "FFI::cdef() expects string $code");
    const char *code = ((TStr *)args[0].u.p)->c;
    const char *lib = NULL;
    char libbuf[512];
    if (n >= 2 && args[1].t == T_STR) {
        snprintf(libbuf, sizeof libbuf, "%s", ((TStr *)args[1].u.p)->c);
        lib = libbuf;
    }
    FFIScope *sc = scope_new(code, lib);
    int idx = scope_reg(sc);
    TObj *o = tphp_obj_alloc(g_ffi_cls);
    o->props[0] = TV_INT(idx);
    return TV_OBJ(o);
}

/* FFI::new —— 静态（无 scope）或 $ffi->new（self 为 FFI 对象） */
static TVal nm_ffi_new(VM *vm, void *self, TVal *args, int n) {
    if (n < 1 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "FFI::new() expects string $type");
    bool owned = n >= 2 ? tval_to_bool(args[1]) : true;
    FFIScope *sc = NULL;
    if (self) {
        TObj *fo = (TObj *)self;
        if (fo->cls == g_ffi_cls && fo->props[0].t == T_INT)
            sc = g_scopes[fo->props[0].u.i];
    }
    FFIType *t = parse_type_string(vm, ((TStr *)args[0].u.p)->c, sc);
    void *buf = tm_malloc(t->size ? t->size : 1);
    memset(buf, 0, t->size ? t->size : 1);
    return cdata_wrap(buf, t, owned, sc);
}

static TVal nm_ffi_memcpy(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 3)
        tphp_vm_raise(vm, "ArgumentCountError", "FFI::memcpy() expects 3 arguments");
    FFCData *dst = cdata_of(vm, args[0], "FFI::memcpy");
    int64_t size = tval_to_int(args[2]);
    if (size < 0)
        tphp_vm_raise(vm, "ValueError", "FFI::memcpy(): size must be >= 0");
    if (args[1].t == T_STR) {
        TStr *s = (TStr *)args[1].u.p;
        if ((size_t)size > s->len) size = s->len;
        memcpy(dst->ptr, s->c, (size_t)size);
    } else if (args[1].t == T_OBJ) {
        FFCData *src = cdata_of(vm, args[1], "FFI::memcpy");
        memcpy(dst->ptr, src->ptr, (size_t)size);
    } else {
        tphp_vm_raise(vm, "TypeError", "FFI::memcpy(): src must be string or CData");
    }
    return TV_NULL;
}

static TVal nm_ffi_free(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError", "FFI::free() expects 1 argument");
    FFCData *d = cdata_of(vm, args[0], "FFI::free");
    if (!d->owned)
        tphp_vm_raise(vm, "Error", "FFI::free(): not an owned CData");
    if (d->ptr) {
        tm_free(d->ptr);
        d->ptr = NULL;
        d->owned = false;
    }
    return TV_NULL;
}

static TVal nm_ffi_sizeof(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError", "FFI::sizeof() expects 1 argument");
    FFIScope *sc = NULL;
    if (self) {
        TObj *fo = (TObj *)self;
        if (fo->cls == g_ffi_cls && fo->props[0].t == T_INT)
            sc = g_scopes[fo->props[0].u.i];
    }
    if (args[0].t == T_OBJ) {
        FFCData *d = cdata_of(vm, args[0], "FFI::sizeof");
        return TV_INT((int64_t)d->type->size);
    }
    if (args[0].t == T_STR) {
        FFIType *t = parse_type_string(vm, ((TStr *)args[0].u.p)->c, sc);
        return TV_INT((int64_t)t->size);
    }
    tphp_vm_raise(vm, "TypeError", "FFI::sizeof() expects string or CData");
    return TV_NULL;
}

static TVal nm_ffi_alignof(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 1 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "FFI::alignof() expects string $type");
    FFIType *t = parse_type_string(vm, ((TStr *)args[0].u.p)->c, NULL);
    return TV_INT((int64_t)t->align);
}

static TVal nm_ffi_cast(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    if (n < 2 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "FFI::cast() expects (string, mixed)");
    FFIScope *sc = NULL;
    FFIType *t = parse_type_string(vm, ((TStr *)args[0].u.p)->c, sc);
    void *p = NULL;
    if (args[1].t == T_NULL) p = NULL;
    else if (args[1].t == T_OBJ) {
        FFCData *d = cdata_of(vm, args[1], "FFI::cast");
        p = d->ptr;
    } else if (args[1].t == T_INT && t->code == FFI_PTR
               && t->array_elem && t->array_elem->code == FFI_VOID) {
        /* void* only: allow int -> opaque pointer (webview_t handle
         * cross-thread pass). char-star / int-star etc still rejected
         * (they enable typed memory access). void* cannot deref. */
        p = (void *)(intptr_t)args[1].u.i;
    } else
        /* reject bare int -> concrete type (ffi-security section 8):
         * int -> pointer is an arbitrary address read/write entry point.
         * Legal addresses come from CData or system API returns;
         * only void-pointer types are exempt. */
        tphp_vm_raise(vm, "TypeError",
                      "FFI::cast(): ptr must be CData, null, or int (void* only)");
    return cdata_wrap(p, t, false, sc);
}

static TVal nm_ffi_offsetof(VM *vm, void *self, TVal *args, int n) {
    FFIScope *sc = NULL;
    if (self) {
        TObj *fo = (TObj *)self;
        if (fo->cls == g_ffi_cls && fo->props[0].t == T_INT)
            sc = g_scopes[fo->props[0].u.i];
    }
    if (n < 2 || args[0].t != T_STR || args[1].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "FFI::offsetof() expects (string, string)");
    FFIType *t = parse_type_string(vm, ((TStr *)args[0].u.p)->c, sc);
    if (t->code != FFI_STRUCT)
        tphp_vm_raise(vm, "TypeError", "FFI::offsetof(): type must be a struct");
    const char *field = ((TStr *)args[1].u.p)->c;
    for (int i = 0; i < t->n_fields; i++)
        if (strcmp(t->fields[i].name, field) == 0)
            return TV_INT((int64_t)t->fields[i].offset);
    tphp_vm_raise(vm, "ValueError", "FFI::offsetof(): unknown field \"%s\"", field);
    return TV_NULL;
}

/* ---- CData 方法 ---- */

static TVal nm_cdata_getvalue(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    FFCData *d = g_cdatas[o->props[0].u.i];
    if (!d->ptr)
        tphp_vm_raise(vm, "Error", "FFI\\CData::getValue(): null pointer");
    FFIType *t = d->type;
    if (t->code == FFI_PTR && t->array_elem) t = t->array_elem;
    switch (t->code) {
    case FFI_BOOL:  return *(uint8_t *)d->ptr ? TV_TRUE : TV_FALSE;
    case FFI_INT8:  return TV_INT(*(int8_t *)d->ptr);
    case FFI_UINT8: return TV_INT(*(uint8_t *)d->ptr);
    case FFI_INT16: return TV_INT(*(int16_t *)d->ptr);
    case FFI_UINT16:return TV_INT(*(uint16_t *)d->ptr);
    case FFI_INT32: return TV_INT(*(int32_t *)d->ptr);
    case FFI_UINT32:return TV_INT(*(uint32_t *)d->ptr);
    case FFI_INT64: return TV_INT(*(int64_t *)d->ptr);
    case FFI_UINT64:return TV_INT((int64_t)*(uint64_t *)d->ptr);
    case FFI_FLOAT: return TV_FLOAT(*(float *)d->ptr);
    case FFI_DOUBLE:return TV_FLOAT(*(double *)d->ptr);
    case FFI_ENUM:  return TV_INT(*(int32_t *)d->ptr);
    case FFI_FUNC_PTR: {
        void *p = *(void **)d->ptr;
        return p ? cdata_wrap(p, t, false, d->scope) : TV_NULL;
    }
    case FFI_VOID:  return TV_NULL;
    case FFI_WCHAR:
#if defined(_WIN32)
        return TV_INT(*(uint16_t *)d->ptr);
#else
        return TV_INT(*(int32_t *)d->ptr);
#endif
    case FFI_PTR: {
        void *p = *(void **)d->ptr;
        if (!p) return TV_NULL;
        if (t->array_elem && t->array_elem->code == FFI_INT8) {
            size_t ln = 0;
            while (((char *)p)[ln]) ln++;
            return TV_STR(tstr_new((char *)p, ln));
        }
        return cdata_wrap(p, t->array_elem ? t->array_elem : bt_void, false, d->scope);
    }
    case FFI_STRUCT: case FFI_ARRAY:
        return cdata_wrap(d->ptr, t, false, d->scope);
    }
    return TV_NULL;
}

static TVal nm_cdata_setvalue(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    FFCData *d = g_cdatas[o->props[0].u.i];
    if (!d->ptr)
        tphp_vm_raise(vm, "Error", "FFI\\CData::setValue(): null pointer");
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError", "FFI\\CData::setValue() expects 1 argument");
    FFIType *t = d->type;
    if (t->code == FFI_PTR && t->array_elem) t = t->array_elem;
    encode_arg(vm, t, args[0], d->ptr, "FFI\\CData::setValue", 0);
    return TV_NULL;
}

static TVal nm_cdata_cast(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    FFCData *d = g_cdatas[o->props[0].u.i];
    if (n < 1 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "FFI\\CData::cast() expects string $type");
    FFIType *t = parse_type_string(vm, ((TStr *)args[0].u.p)->c, d->scope);
    return cdata_wrap(d->ptr, t, false, d->scope);
}

static TVal nm_cdata_getaddr(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    FFCData *d = g_cdatas[o->props[0].u.i];
    return TV_INT((int64_t)(intptr_t)d->ptr);
}

static TVal nm_cdata_getsize(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    FFCData *d = g_cdatas[o->props[0].u.i];
    return TV_INT((int64_t)d->type->size);
}

/* ---- VM 钩子 ---- */

bool tphp_ffi_try_mcall(VM *vm, TObj *o, TStr *mname, void *recv) {
    (void)vm;
    if (o->cls != g_ffi_cls || o->props[0].t != T_INT) return false;
    FFIScope *sc = g_scopes[o->props[0].u.i];
    FFIFunc *fn = scope_find_func(sc, tstr_val(mname));
    if (!fn) return false;
    VMCallRec *rec = (VMCallRec *)recv;
    rec->is_builtin = true;
    rec->ffi_fn = fn;
    rec->is_method = true;
    rec->this_obj = o;
    tval_addref(TV_OBJ(o));
    return true;
}

TVal tphp_ffi_call_fn(VM *vm, void *fn, TVal *args, int n) {
    return ffi_do_call(vm, (FFIFunc *)fn, args, n);
}

bool tphp_ffi_fetch_prop(VM *vm, TObj *o, TStr *name, TVal *out) {
    /* FFI 句柄对象：暴露 cdef 中的 enum 常量（$ffi->RED） */
    if (o->cls == g_ffi_cls && o->props[0].t == T_INT) {
        FFIScope *sc = g_scopes[o->props[0].u.i];
        if (!sc) return false;
        const char *n = tstr_val(name);
        for (int i = 0; i < sc->n_consts; i++) {
            if (strcmp(sc->consts[i].name, n) == 0) {
                *out = TV_INT(sc->consts[i].val);
                return true;
            }
        }
        return false;   /* 未命中常量 → 走正常属性路径（报 Undefined） */
    }
    if (o->cls != g_cdata_cls || o->props[0].t != T_INT) return false;
    FFCData *d = g_cdatas[o->props[0].u.i];
    if (!d->ptr) {
        tphp_vm_raise(vm, "Error", "Cannot access field of null CData");
        return false;
    }
    FFIType *t = d->type;
    if (t->code == FFI_PTR && t->array_elem && t->array_elem->code == FFI_STRUCT)
        t = t->array_elem;
    if (t->code != FFI_STRUCT) return false;
    const char *fn = tstr_val(name);
    for (int i = 0; i < t->n_fields; i++) {
        if (strcmp(t->fields[i].name, fn) != 0) continue;
        FFIType *ft = t->fields[i].type;
        char *fp = (char *)d->ptr + t->fields[i].offset;
        if (ft->code == FFI_ARRAY || ft->code == FFI_STRUCT) {
            *out = cdata_wrap(fp, ft, false, d->scope);
            return true;
        }
        if (ft->code == FFI_PTR) {
            void *p = *(void **)fp;
            if (ft->array_elem && ft->array_elem->code == FFI_INT8 && p) {
                size_t ln = 0;
                while (((char *)p)[ln]) ln++;
                *out = TV_STR(tstr_new((char *)p, ln));
                return true;
            }
            if (ft->array_elem && ft->array_elem->code == FFI_WCHAR && p) {
                *out = TV_STR(wide_to_utf8(p, 4096));
                return true;
            }
            *out = p ? cdata_wrap(p, ft->array_elem ? ft->array_elem : bt_void,
                                  false, d->scope)
                     : TV_NULL;
            return true;
        }
        switch (ft->code) {
        case FFI_BOOL:  *out = *(uint8_t *)fp ? TV_TRUE : TV_FALSE; return true;
        case FFI_INT8:  *out = TV_INT(*(int8_t *)fp); return true;
        case FFI_UINT8: *out = TV_INT(*(uint8_t *)fp); return true;
        case FFI_INT16: *out = TV_INT(*(int16_t *)fp); return true;
        case FFI_UINT16:*out = TV_INT(*(uint16_t *)fp); return true;
        case FFI_INT32: *out = TV_INT(*(int32_t *)fp); return true;
        case FFI_UINT32:*out = TV_INT(*(uint32_t *)fp); return true;
        case FFI_INT64: *out = TV_INT(*(int64_t *)fp); return true;
        case FFI_UINT64:*out = TV_INT((int64_t)*(uint64_t *)fp); return true;
        case FFI_FLOAT: *out = TV_FLOAT(*(float *)fp); return true;
        case FFI_DOUBLE:*out = TV_FLOAT(*(double *)fp); return true;
        case FFI_ENUM:  *out = TV_INT(*(int32_t *)fp); return true;
        case FFI_FUNC_PTR: {
            /* v2①：字段里的函数指针读回为非 owned CData 视图（同
             * getValue；trampoline 地址由 TLS 表保活到线程结束） */
            void *p = *(void **)fp;
            *out = p ? cdata_wrap(p, ft, false, d->scope) : TV_NULL;
            return true;
        }
        case FFI_WCHAR:
#ifdef _WIN32
            *out = TV_INT(*(uint16_t *)fp);
#else
            *out = TV_INT(*(uint32_t *)fp);
#endif
            return true;
        default: return false;
        }
    }
    return false;
}

bool tphp_ffi_assign_prop(VM *vm, TObj *o, TStr *name, TVal val) {
    if (o->cls != g_cdata_cls || o->props[0].t != T_INT) return false;
    FFCData *d = g_cdatas[o->props[0].u.i];
    if (!d->ptr) {
        tphp_vm_raise(vm, "Error", "Cannot assign field of null CData");
        return false;
    }
    FFIType *t = d->type;
    if (t->code == FFI_PTR && t->array_elem && t->array_elem->code == FFI_STRUCT)
        t = t->array_elem;
    if (t->code != FFI_STRUCT) return false;
    const char *fn = tstr_val(name);
    for (int i = 0; i < t->n_fields; i++) {
        if (strcmp(t->fields[i].name, fn) != 0) continue;
        FFIType *ft = t->fields[i].type;
        char *fp = (char *)d->ptr + t->fields[i].offset;
        if (ft->code == FFI_STRUCT) {
            if (val.t == T_OBJ) {
                TObj *so = (TObj *)val.u.p;
                if (so->cls == g_cdata_cls && so->props[0].t == T_INT) {
                    memcpy(fp, g_cdatas[so->props[0].u.i]->ptr, ft->size);
                    return true;
                }
            }
            tphp_vm_raise(vm, "TypeError", "Cannot assign non-CData to struct field");
            return false;
        }
        if (ft->code == FFI_PTR) {
            if (val.t == T_NULL) { *(void **)fp = NULL; return true; }
            if (val.t == T_STR) {
                TStr *s = (TStr *)val.u.p;
                if (ft->array_elem && ft->array_elem->code == FFI_WCHAR) {
                    size_t nels = 0;
                    void *w = utf8_to_wide(s->c, s->len, &nels);
                    /* 注意：字段持有的 wide 缓冲需长期存活——简化为直接
                     * 存 UTF-8 指针（调用方保证字符串存活）或报错。
                     * 完整方案需给 CData 加 owned wide 缓冲表；M5.5 先
                     * 用 UTF-8 直存，文档注明 wchar 字段赋值限制。 */
                    tm_free(w);   /* 不使用临时转换 */
                    *(void **)fp = (void *)s->c;
                    return true;
                }
                *(void **)fp = (void *)s->c;
                return true;
            }
            if (val.t == T_OBJ) {
                TObj *po = (TObj *)val.u.p;
                if (po->cls == g_cdata_cls && po->props[0].t == T_INT) {
                    *(void **)fp = g_cdatas[po->props[0].u.i]->ptr;
                    return true;
                }
            }
            tphp_vm_raise(vm, "TypeError", "Cannot assign value to pointer field");
            return false;
        }
        if (ft->code == FFI_ARRAY) return false;
        encode_arg(vm, ft, val, fp, "FFI\\CData", i);
        return true;
    }
    return false;
}

bool tphp_ffi_fetch_dim(VM *vm, TVal base, TVal key, TVal *out) {
    if (base.t != T_OBJ) return false;
    TObj *o = (TObj *)base.u.p;
    if (o->cls != g_cdata_cls || o->props[0].t != T_INT) return false;
    FFCData *d = g_cdatas[o->props[0].u.i];
    if (!d->ptr) {
        tphp_vm_raise(vm, "Error", "Cannot index null CData");
        return false;
    }
    if (key.t != T_INT) {
        tphp_vm_raise(vm, "TypeError", "CData offset must be int");
        return false;
    }
    int64_t idx = key.u.i;
    FFIType *t = d->type;
    FFIType *el;
    size_t stride;
    if (t->code == FFI_ARRAY) {
        el = t->array_elem;
        stride = el->size;
        if (idx < 0 || (size_t)idx >= t->nelem) {
            tphp_vm_raise(vm, "Error", "CData array index %lld out of bounds",
                          (long long)idx);
            return false;
        }
    } else if (t->code == FFI_PTR && t->array_elem) {
        el = t->array_elem;
        stride = el->size ? el->size : 1;
        if (idx < 0) {
            tphp_vm_raise(vm, "Error", "CData pointer index must be >= 0");
            return false;
        }
    } else {
        return false;
    }
    char *ep = (char *)d->ptr + (size_t)idx * (ptrdiff_t)stride;
    if (el->code == FFI_ARRAY || el->code == FFI_STRUCT) {
        *out = cdata_wrap(ep, el, false, d->scope);
        return true;
    }
    if (el->code == FFI_FUNC_PTR) {
        /* v2①：函数指针数组元素读回（同字段；非 owned 视图） */
        void *p = *(void **)ep;
        *out = p ? cdata_wrap(p, el, false, d->scope) : TV_NULL;
        return true;
    }
    if (el->code == FFI_PTR) {
        void *p = *(void **)ep;
        *out = p ? cdata_wrap(p, bt_void, false, d->scope) : TV_NULL;
        return true;
    }
    switch (el->code) {
    case FFI_BOOL:  *out = *(uint8_t *)ep ? TV_TRUE : TV_FALSE; return true;
    case FFI_INT8:  *out = TV_INT(*(int8_t *)ep); return true;
    case FFI_UINT8: *out = TV_INT(*(uint8_t *)ep); return true;
    case FFI_INT16: *out = TV_INT(*(int16_t *)ep); return true;
    case FFI_UINT16:*out = TV_INT(*(uint16_t *)ep); return true;
    case FFI_INT32: *out = TV_INT(*(int32_t *)ep); return true;
    case FFI_UINT32:*out = TV_INT(*(uint32_t *)ep); return true;
    case FFI_INT64: *out = TV_INT(*(int64_t *)ep); return true;
    case FFI_UINT64:*out = TV_INT((int64_t)*(uint64_t *)ep); return true;
    case FFI_FLOAT: *out = TV_FLOAT(*(float *)ep); return true;
    case FFI_DOUBLE:*out = TV_FLOAT(*(double *)ep); return true;
    case FFI_ENUM:  *out = TV_INT(*(int32_t *)ep); return true;
    case FFI_WCHAR:
#ifdef _WIN32
        *out = TV_INT(*(uint16_t *)ep);
#else
        *out = TV_INT(*(uint32_t *)ep);
#endif
        return true;
    default: return false;
    }
}

bool tphp_ffi_assign_dim(VM *vm, TVal base, TVal key, TVal val) {
    if (base.t != T_OBJ) return false;
    TObj *o = (TObj *)base.u.p;
    if (o->cls != g_cdata_cls || o->props[0].t != T_INT) return false;
    FFCData *d = g_cdatas[o->props[0].u.i];
    if (!d->ptr) {
        tphp_vm_raise(vm, "Error", "Cannot index null CData");
        return false;
    }
    if (key.t != T_INT) {
        tphp_vm_raise(vm, "TypeError", "CData offset must be int");
        return false;
    }
    int64_t idx = key.u.i;
    FFIType *t = d->type;
    FFIType *el;
    size_t stride;
    if (t->code == FFI_ARRAY) {
        el = t->array_elem;
        stride = el->size;
        if (idx < 0 || (size_t)idx >= t->nelem) {
            tphp_vm_raise(vm, "Error", "CData array index %lld out of bounds",
                          (long long)idx);
            return false;
        }
    } else if (t->code == FFI_PTR && t->array_elem) {
        el = t->array_elem;
        stride = el->size ? el->size : 1;
        if (idx < 0) {
            tphp_vm_raise(vm, "Error", "CData pointer index must be >= 0");
            return false;
        }
    } else {
        return false;
    }
    char *ep = (char *)d->ptr + (size_t)idx * (ptrdiff_t)stride;
    if (el->code == FFI_STRUCT) {
        if (val.t == T_OBJ) {
            TObj *so = (TObj *)val.u.p;
            if (so->cls == g_cdata_cls && so->props[0].t == T_INT) {
                memcpy(ep, g_cdatas[so->props[0].u.i]->ptr, el->size);
                return true;
            }
        }
        tphp_vm_raise(vm, "TypeError", "Cannot assign non-CData to struct element");
        return false;
    }
    if (el->code == FFI_PTR) {
        if (val.t == T_NULL) { *(void **)ep = NULL; return true; }
        if (val.t == T_STR) {
            *(void **)ep = (void *)((TStr *)val.u.p)->c;
            return true;
        }
        if (val.t == T_OBJ) {
            TObj *po = (TObj *)val.u.p;
            if (po->cls == g_cdata_cls && po->props[0].t == T_INT) {
                *(void **)ep = g_cdatas[po->props[0].u.i]->ptr;
                return true;
            }
        }
        tphp_vm_raise(vm, "TypeError", "Cannot assign value to pointer element");
        return false;
    }
    encode_arg(vm, el, val, ep, "FFI\\CData", (int)idx);
    return true;
}

bool tphp_ffi_is_cdata_cls(TClass *cls) { return cls == g_cdata_cls; }
bool tphp_ffi_is_ffi_cls(TClass *cls) { return cls == g_ffi_cls; }

/* ---- 类注册 ---- */

static void add_native(TClass *cls, const char *name, void *native, bool is_static) {
    TMethod *ms = tm_malloc(sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    if (cls->methods) {
        memcpy(ms, cls->methods, sizeof(TMethod) * (size_t)cls->n_methods);
        tm_free(cls->methods);
    }
    TMethod *m = &ms[cls->n_methods++];
    memset(m, 0, sizeof *m);
    m->name = tstr_intern(name, strlen(name));
    m->visibility = 0;
    m->native = native;
    m->is_static = is_static;
    cls->methods = ms;
}

static TClass *make_class(const char *name) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern(name, strlen(name));
    cls->kind = 0;
    cls->flattened = true;
    cls->m_ctor = -1; cls->m_dtor = -1; cls->m_get = -1; cls->m_set = -1;
    cls->m_isset = -1; cls->m_unset = -1; cls->m_call = -1;
    cls->m_callstatic = -1; cls->m_tostring = -1; cls->m_invoke = -1;
    cls->m_clone = -1; cls->m_debuginfo = -1; cls->m_clonestate = -1;
    TProp *p = tm_malloc(sizeof(TProp));
    memset(p, 0, sizeof *p);
    p->name = tstr_intern("__handle", 8);
    p->defval = TV_NULL;
    p->slot = 0;
    cls->props = p;
    cls->n_props = 1;
    return cls;
}

static _Thread_local bool g_ffi_inited;

void tphp_ffi_init(void) {
    if (g_ffi_inited) return;
    g_ffi_inited = true;
    builtin_types_init();

    TClass *ffi = make_class("FFI");
    add_native(ffi, "cdef", (void *)nm_ffi_cdef, true);
    add_native(ffi, "new", (void *)nm_ffi_new, true);
    add_native(ffi, "memcpy", (void *)nm_ffi_memcpy, true);
    add_native(ffi, "free", (void *)nm_ffi_free, true);
    add_native(ffi, "sizeof", (void *)nm_ffi_sizeof, true);
    add_native(ffi, "alignof", (void *)nm_ffi_alignof, true);
    add_native(ffi, "cast", (void *)nm_ffi_cast, true);
    add_native(ffi, "offsetof", (void *)nm_ffi_offsetof, true);
    tphp_class_register(ffi);
    g_ffi_cls = ffi;

    TClass *cd = make_class("FFI\\CData");
    cd->native_dtor = cdata_dtor;
    add_native(cd, "getValue", (void *)nm_cdata_getvalue, false);
    add_native(cd, "setValue", (void *)nm_cdata_setvalue, false);
    add_native(cd, "cast", (void *)nm_cdata_cast, false);
    add_native(cd, "getAddress", (void *)nm_cdata_getaddr, false);
    add_native(cd, "getSize", (void *)nm_cdata_getsize, false);
    tphp_class_register(cd);
    g_cdata_cls = cd;
}
