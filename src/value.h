/*
 * TinyPHP.rt - a small PHP runtime
 * value.h - core value types: TVal (16 bytes), strings, arrays, GC header
 *
 * Ownership rules (see PLAN/03 §2):
 *  - TVal slots (CV/TMP) release their old value before being overwritten
 *  - a frame releases all its slots when destroyed
 *  - constant pool values are program-lifetime (interned-like)
 *  - heap objects (TStr/TArr/...) carry a GCHead with rc; the mark-sweep GC
 *    in mem.c only reclaims objects kept alive solely by cycles
 */
#ifndef TPHP_VALUE_H
#define TPHP_VALUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* POSIX strcasecmp* on MSVC/UCRT (Windows SDK has no strncasecmp;
 * mingw provides it natively, so only _MSC_VER-targeting clang needs this) */
#if defined(_MSC_VER)
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#endif

/* ---- type tags ---- */
typedef enum {
    T_NULL = 0, T_FALSE, T_TRUE, T_INT, T_FLOAT, T_STR, T_ARR,
    T_OBJ, T_CLOSURE, T_ENUM_CASE, T_RES,
} TType;

typedef struct TVal { union { int64_t i; double d; void *p; } u; TType t; } TVal;

/* internal type tag: never a user value. Marks undefined slots and free
 * data[] slots in arrays (freelist links live in u.i). */
#define T_UNDEF ((TType)(T_RES + 1))

/* ---- allocator (mem.c, mimalloc wrapper; exits on OOM) ---- */
void *tm_malloc(size_t n);
void *tm_realloc(void *p, size_t n);
void   tm_free(void *p);

/* ---- GC header (heap objects) ---- */
enum { GC_COLOR_UNMARKED = 0, GC_COLOR_BLACK, GC_COLOR_DEAD };
enum { GC_KIND_STR = 1, GC_KIND_ARR = 2, GC_KIND_CLO = 3, GC_KIND_EX = 4,
       GC_KIND_OBJ = 5 };
enum { GC_FLAG_INTERNED = 1, GC_FLAG_DTORED = 2, GC_FLAG_MFCC = 4 };

typedef struct GCHead {
    uint32_t rc;
    uint8_t  color;
    uint8_t  kind;
    uint8_t  flags;
    struct GCHead *gc_prev, *gc_next;   /* global allocation registry */
} GCHead;

/* ---- exception classes (M2 built-in hierarchy; user classes M3) ---- */
typedef struct TExClass {
    const char *name;
    const struct TExClass *parent;   /* NULL = Throwable root */
} TExClass;

/* ---- exception object (TVal type T_OBJ) ---- */
typedef struct TExObj {
    GCHead h;
    const TExClass *cls;
    struct TStr *message;
    int64_t code;
    struct TExObj *previous;
    int line;                 /* throw site (0 = raised without an op line) */
    struct TObj *wrap;        /* P1-2: 用户异常类实例（throw 的 TObj 包装成
                               * TExObj 走统一异常机制；catch 绑定/取消息
                               * 时解包回用户对象）。NULL = 引擎异常 */
} TExObj;

/* ---- closure value (TVal type T_CLOSURE): compiled TFunc + captured
 * values. Capture slots live at the front of the call frame.
 * bfn != NULL marks a first-class wrapper around a builtin function
 * (fn is NULL then); user closures have fn set and bfn NULL. ---- */
typedef struct TClosure {
    GCHead h;
    struct TFunc *fn;
    void *bfn;               /* BuiltinFn when wrapping a builtin, else NULL */
    TVal *captures;
    uint16_t n_cap;
    struct TClass *scope;    /* P1-3: bindTo/bind 重绑的作用域类；NULL =
                                继承 fn->decl_cls（创建处作用域） */
} TClosure;

void tclo_free(TClosure *c);   /* mem.c */
void tex_free(TExObj *x);

/* ---- string: immutable, NUL-terminated for C interop ---- */
typedef struct TStr { GCHead h; uint32_t len; uint32_t hash; char c[]; } TStr;

/* ---- array: insertion-ordered hash map (PHP array semantics) ----
 * data[] holds values in insertion order; free slots hold T_NULL with the
 * freelist next-index in u.i. buckets[] is linear-probe open addressing:
 * key.t == T_NULL means never-used, val_off == UINT32_MAX means tombstone
 * (a valid key can never be T_NULL because null keys normalize to ""). */
typedef struct { TVal key; uint32_t val_off; } ABucket;

typedef struct TArr {
    GCHead h;
    uint32_t n;          /* live element count */
    uint32_t bcap;       /* bucket capacity, power of two */
    uint32_t hmask;      /* bcap - 1 */
    int64_t  next_idx;   /* next auto-index key */
    ABucket *buckets;
    TVal    *data;
    uint32_t dcap, dused;
    uint32_t free_head;  /* freelist head of free data slots (-1 = none) */
} TArr;

/* ---- classes & objects (M3; declared here, implemented in class.c) ----
 * TClass is program-lifetime (never freed, not GC-tracked); TObj is a heap
 * value with a GCHead (TVal type T_OBJ). Exception objects keep their own
 * lightweight TExObj representation (GC_KIND_EX) — FETCH_PROP / method calls
 * dispatch on the GC kind. */
struct TFunc;   /* opcode.h */

typedef enum { CK_CLASS = 0, CK_INTERFACE, CK_TRAIT, CK_ENUM } ClassKind;
enum { CVIS_PUBLIC = 0, CVIS_PROTECTED = 1, CVIS_PRIVATE = 2 };
enum { CLS_ABSTRACT = 1, CLS_FINAL = 2, CLS_READONLY = 4 };   /* class flags */

typedef struct TProp {
    TStr *name;            /* interned, without $ */
    TStr *type;            /* declared type, normalized; NULL = untyped (P1-1) */
    struct TClass *decl;   /* 声明类（readonly 写入作用域判定用；P3） */
    uint8_t visibility;
    bool is_static;
    bool is_readonly;
    TVal defval;           /* T_UNDEF = required (constructor must set) */
    uint16_t def_thunk;    /* compiled constant-expression thunk, 0 = none */
    uint16_t slot;         /* instance property slot (declaration order) */
} TProp;

typedef struct TMethod {
    TStr *name;            /* interned method name (case preserved) */
    struct TFunc *fn;      /* NULL for abstract / interface declarations */
    uint8_t visibility;
    bool is_static;
    bool is_abstract;
    bool is_final;
    void *native;          /* M4: NativeMethodFn (opcode.h) for internal
                              classes (Async\Coroutine 等) — fn 为 NULL 时生效 */
    /* P2-A named args: AST parameter names (same parse-interned TStr* as
     * AArg.name, pointer-comparable); NULL for native/abstract-in-trait */
    TStr **param_names; uint16_t n_params;
} TMethod;

typedef struct TEnumCase {
    TStr *name;            /* case identifier */
    TVal backing;          /* backed enum: int/string value; unit enum: T_NULL */
    struct TObj *obj;      /* singleton instance (lazy) */
} TEnumCase;

typedef struct TClass {
    TStr *name;            /* fully-qualified class name, interned */
    struct TClass *parent;
    TStr *parent_name;     /* unresolved parent FQCN (cleared on flatten) */
    ClassKind kind;
    uint8_t flags;         /* CLS_* */
    bool flattened;
    /* traits to bind at flatten time (unresolved FQCNs) */
    TStr **trait_names; int n_trait_names;
    void *trait_mods;      /* AModList* (ast.h) — as/insteadof rules */
    /* flattened member tables (own + inherited + traits, parent-first) */
    TMethod *methods; int n_methods;
    TProp *props; int n_props;            /* instance props; statics below */
    TProp *sprops; int n_sprops;          /* static props (per-class storage) */
    TVal *sprop_vals;
    struct TClass **ifaces; int n_ifaces; /* all interfaces, flattened */
    TStr **iface_names; int n_iface_names; /* unresolved (own + flat) */
    /* class constants: parallel arrays, declaration order.
     * cthunks[i] != 0：惰性常量表达式（compile_thunk 索引），cvals[i] 为
     * T_UNDEF 占位，首次取用时求值并缓存（P2-B 类常量表达式）。 */
    TStr **cnames; TVal *cvals; uint16_t *cthunks; int n_consts;
    /* enum */
    TEnumCase *cases; int n_cases;        /* CK_ENUM */
    bool backed;          /* enum has a backing type */
    uint8_t backing_type; /* T_INT or T_STR */
    /* quick links into methods[] (flattened), -1 when absent */
    int16_t m_ctor, m_dtor, m_get, m_set, m_isset, m_unset,
            m_call, m_callstatic, m_tostring, m_invoke, m_clone, m_debuginfo,
            m_clonestate;
    /* M5.5 FFI: native destructor for internal classes that own native
     * resources (FFI\CData owned buffers). Called from tobj_free. */
    void (*native_dtor)(struct TObj *o);
} TClass;

typedef struct TObj {
    GCHead h;
    TClass *cls;
    TVal *props;           /* n_props slots, slot order per TClass::props */
    uint32_t oid;          /* creation-order id (var_dump object#N) */
} TObj;

void tobj_free(TObj *o);   /* mem.c */
/* vm.c: 运行 __destruct（若类声明了且未运行过）。异常在析构器内抛出时
 * 按 PHP 语义就地 fatal（exit 255），不会从释放点向外传播。 */
void tphp_obj_run_dtor(TObj *o);
/* vm.c: 请求收尾——对全部带 __destruct 的存活对象跑一遍（逆创建序近似） */
void tphp_run_shutdown_dtors(void);
/* mem.c: GC 注册表头（上述收尾遍历用） */
struct GCHead *tphp_gc_registry(void);

/* __debugInfo() dispatch for var_dump (vm.c): returns the method's array
 * with a +1 reference, or TV_NULL when the class has no __debugInfo */
TVal tphp_vm_debug_info(TObj *o);

/* ---- constructors ----
 * (parameter names avoid the member tokens: TV_INT(i) would corrupt .u.i) */
extern TVal TV_NULL, TV_FALSE, TV_TRUE;
#define TV_INT(iv)    ((TVal){ .u.i = (int64_t)(iv), .t = T_INT })
#define TV_FLOAT(dv)  ((TVal){ .u.d = (double)(dv),   .t = T_FLOAT })
#define TV_STR(pv)    ((TVal){ .u.p = (void*)(pv),    .t = T_STR })
#define TV_ARR(pv)    ((TVal){ .u.p = (void*)(pv),    .t = T_ARR })
#define TV_OBJ(pv)    ((TVal){ .u.p = (void*)(pv),    .t = T_OBJ })
#define TV_CLO(pv)    ((TVal){ .u.p = (void*)(pv),    .t = T_CLOSURE })

static inline bool tval_is_scalar(TVal v) {
    return v.t <= T_FLOAT || v.t == T_STR;
}

/* ---- refcounting ----
 * T_STR / T_ARR / T_OBJ(exceptions) / T_CLOSURE carry a GCHead; everything
 * else is a raw value. Interned strings (program-literal pool) are shared
 * read-only across threads (M5) — refcount ops on them are no-ops. */
void tval_release(TVal *v);
static inline void tval_addref(TVal v) {
    if (v.t < T_STR || v.t > T_CLOSURE) return;
    GCHead *h = (GCHead *)v.u.p;
    if (h->flags & GC_FLAG_INTERNED) return;
    h->rc++;
}

/* deep-copy a value for cross-thread transfer / class-table cloning
 * (serialize.c): scalars pass through, interned strings are shared, heap
 * strings/arrays are recursively copied; objects/closures raise
 * NotCloneableError semantics at the caller level (checked here via flag) */
TVal tval_clone_deep(TVal v);

/* registry insertion (vm.c builds closures/exception objects directly) */
void gc_register(GCHead *h);

/* ---- heap allocation (mem.c) ---- */
TStr *tstr_alloc_raw(uint32_t len, bool interned);
TArr *tarr_alloc_raw(void);
void  tstr_free(TStr *s);
/* mark-sweep pass reclaiming reference cycles; returns objects freed */
int64_t gc_collect(void);

/* ---- strings (string.c) ---- */
TStr  *tstr_new(const char *s, size_t len);        /* heap string */
TStr  *tstr_intern(const char *s, size_t len);     /* program-lifetime string */
TStr  *tstr_concat(TStr *a, TStr *b);              /* new heap string */
uint32_t tstr_hash(TStr *s);
static inline size_t tstr_len(TStr *s) { return s->len; }
static inline const char *tstr_val(TStr *s) { return s->c; }
bool tstr_eq(TStr *a, TStr *b);                    /* pointer-or-content equality */

/* number -> string formatting, PHP 8 semantics (precision=14, zend_gcvt) */
TStr *tphp_int_to_str(int64_t i);
TStr *tphp_float_to_str(double d);

/* string -> number classification
 * is_numeric_str: full PHP is_numeric (leading+trailing whitespace ok, no hex)
 * prefix_numeric: PHP arithmetic rule (leading numeric is enough, "123abc"->124)
 * prefix_int:     (int) cast rule, truncates at first non-digit */
bool tstr_is_numeric(const char *s, size_t len, bool *is_int, int64_t *iv, double *dv);
bool tstr_prefix_numeric(const char *s, size_t len, bool *is_int, int64_t *iv, double *dv);
int64_t tstr_prefix_int(const char *s, size_t len);
double tstr_prefix_float(const char *s, size_t len);

/* ---- arrays (array.c) ----
 * keys are normalized at the boundary: int/bool/null/float->int, numeric
 * strings ("8" -> int 8). Returns false when the key type is illegal. */
TArr *tarr_new(void);
void  tarr_free(TArr *a);
bool  tarr_set(TArr *a, TVal key, TVal val);       /* inserts or replaces */
bool  tarr_append(TArr *a, TVal val);              /* $a[] = v */
TVal *tarr_get(TArr *a, TVal key);                 /* NULL when absent */
bool  tarr_has(TArr *a, TVal key);
bool  tarr_unset(TArr *a, TVal key);
static inline uint32_t tarr_count(TArr *a) { return a->n; }
/* normalize a prospective key in place (int / string); false = illegal key */
bool tval_norm_key(TVal *key);
/* copy-on-write: when rc > 1 returns a fresh one-level copy (element values
 * are shared, refcounted) and leaves `a`'s refcount untouched — the caller
 * must swap its slot: release old, store the returned array. rc <= 1
 * returns `a` itself and the caller may mutate in place. */
TArr *tarr_cow(TArr *a);

/* ---- conversions (value.c) ---- */
bool    tval_to_bool(TVal v);
int64_t tval_to_int(TVal v);                       /* may raise (NAN/OOB per 8.5) */
double  tval_to_float(TVal v);
TStr   *tval_to_str(TVal v);                       /* new reference, caller releases */

/* __toString hook: the VM installs it so object-to-string conversion can
 * run the __toString method. Returns a +1 TStr reference; raises when the
 * object has no __toString or the method result is not a string. */
typedef TStr *(*ToStrHook)(TVal v);
extern _Thread_local ToStrHook tphp_tostr_hook;

/* ---- comparison (value.c), PHP 8 loose semantics ----
 * returns -1/0/1; array vs scalar => 1 (array is always "greater") */
int  tval_compare(TVal a, TVal b);
bool tval_loose_eq(TVal a, TVal b);
bool tval_strict_eq(TVal a, TVal b);

/* ---- serialize (serialize.c, M5-b) ----
 * 自有紧凑二进制格式（与 PHP serialize 文本不互通）。对象走 __cloneState
 * 协议；closure/资源抛 NotCloneableError；循环引用抛 Error。
 * serialize 返回 +1 TStr；unserialize 成功 *out = +1 值，格式错返回 false
 * （调用方 warning + false，对齐 PHP）。可能经 raise 跳出（异常机制）。 */
struct VM;
TStr *tphp_serialize_val(struct VM *vm, TVal v);
bool  tphp_unserialize_val(struct VM *vm, const char *d, size_t n, TVal *out);

/* ---- arithmetic (value.c), PHP 8 semantics with numeric strings ----
 * every function may raise a fatal (TypeError/DivisionByZeroError/...) */
TVal tval_add(TVal a, TVal b);
TVal tval_sub(TVal a, TVal b);
TVal tval_mul(TVal a, TVal b);
TVal tval_div(TVal a, TVal b);
TVal tval_mod(TVal a, TVal b);
TVal tval_pow(TVal a, TVal b);
TVal tval_concat(TVal a, TVal b);
TVal tval_shl(TVal a, TVal b);
TVal tval_shr(TVal a, TVal b);
TVal tval_band(TVal a, TVal b);
TVal tval_bor(TVal a, TVal b);
TVal tval_bxor(TVal a, TVal b);
TVal tval_bool_not(TVal v);
TVal tval_neg(TVal v);
TVal tval_pos(TVal v);
TVal tval_bit_not(TVal v);
/* ++/-- ; INC on NULL gives 1, DEC on NULL stays NULL */
TVal tval_inc(TVal v);
TVal tval_dec(TVal v);

#endif /* TPHP_VALUE_H */
