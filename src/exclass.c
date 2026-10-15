/*
 * TinyPHP.rt - exclass.c - P1-2 用户异常类体系
 *
 * 引擎异常走 TExObj + C 侧 TExClass 快路（vm.c tphp_ex_classes）；用户
 * 异常类（class MyErr extends Exception）需要一套真的 TClass 才能
 * extends / catch / class_exists / $e->getMessage()。本文件把同一套层级
 * 注册为内部 TClass（名字与 TExClass 一一对应），throw 用户异常对象时包成
 * TExObj（wrap 字段）走统一异常机制，catch 按名字跨两个世界匹配。
 *
 * 注册必须在 tphp_compile 之前（A_NEW 有编译期类存在性检查）。
 */
#include "class.h"
#include "tphp.h"
#include <string.h>
#include <stdlib.h>

/* ---- native 方法（self = TObj*，内部异常类实例） ---- */

static TVal ex_prop_get(TObj *o, const char *name) {
    int slot = tphp_class_find_prop(o->cls, name);
    if (slot < 0) return TV_NULL;
    TVal v = o->props[slot];
    tval_addref(v);
    return v;
}

static TVal nm_ex_get_message(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return ex_prop_get((TObj *)self, "message");
}
static TVal nm_ex_get_code(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return ex_prop_get((TObj *)self, "code");
}
static TVal nm_ex_get_previous(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return ex_prop_get((TObj *)self, "previous");
}
static TVal nm_ex_get_line(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return ex_prop_get((TObj *)self, "line");
}

/* __construct(message = "", code = 0, previous = null) */
static TVal nm_ex_construct(VM *vm, void *self, TVal *args, int n) {
    (void)vm;
    TObj *o = (TObj *)self;
    TVal msg = (n >= 1 && args[0].t == T_STR) ? args[0] : TV_STR(tstr_new("", 0));
    TVal code = (n >= 2 && args[1].t == T_INT) ? args[1] : TV_INT(0);
    TVal prev = (n >= 3 && args[2].t == T_OBJ) ? args[2] : TV_NULL;
    int sm = tphp_class_find_prop(o->cls, "message");
    int sc = tphp_class_find_prop(o->cls, "code");
    int sp = tphp_class_find_prop(o->cls, "previous");
    if (sm >= 0) { tval_addref(msg); tval_release(&o->props[sm]); o->props[sm] = msg; }
    if (sc >= 0) { tval_addref(code); tval_release(&o->props[sc]); o->props[sc] = code; }
    if (sp >= 0) { tval_addref(prev); tval_release(&o->props[sp]); o->props[sp] = prev; }
    return TV_NULL;
}

/* ---- 注册辅助 ---- */

static void add_native_method(TClass *cls, const char *name, void *fn) {
    TMethod *ms = tm_malloc(sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    if (cls->methods)
        memcpy(ms, cls->methods, sizeof(TMethod) * (size_t)cls->n_methods);
    TMethod *m = &ms[cls->n_methods++];
    memset(m, 0, sizeof *m);
    m->name = tstr_new(name, strlen(name));
    m->visibility = 0;
    m->native = fn;
    tm_free(cls->methods);
    cls->methods = ms;
}

static TProp *ex_props(int *n) {
    /* message / code / previous / line —— 与 PHP 异常属性对应（tphp 既有
     * TExObj 快路同样暴露这四个，见 vm.c ex_getter 白名单） */
    static const char *names[4] = { "message", "code", "previous", "line" };
    TProp *ps = tm_malloc(sizeof(TProp) * 4);
    for (int i = 0; i < 4; i++) {
        memset(&ps[i], 0, sizeof ps[i]);
        ps[i].name = tstr_new(names[i], strlen(names[i]));
        ps[i].visibility = 0;            /* public：与 ex_getter 白名单一致 */
        ps[i].defval = TV_NULL;
        ps[i].slot = (uint16_t)i;
    }
    *n = 4;
    return ps;
}

static TClass *ex_new_class(const char *name, int kind) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern(name, strlen(name));
    cls->kind = (ClassKind)kind;
    cls->m_ctor = cls->m_dtor = cls->m_get = cls->m_set = cls->m_isset =
        cls->m_unset = cls->m_call = cls->m_callstatic = cls->m_tostring =
        cls->m_invoke = cls->m_clone = cls->m_debuginfo = cls->m_clonestate = -1;
    return cls;
}

static void ex_set_parent(TClass *cls, const char *parent) {
    cls->parent_name = tstr_intern(parent, strlen(parent));
}

static void ex_add_iface(TClass *cls, const char *iface) {
    cls->iface_names = tm_realloc(cls->iface_names,
                                  sizeof(TStr *) * (size_t)(cls->n_iface_names + 1));
    cls->iface_names[cls->n_iface_names++] = tstr_intern(iface, strlen(iface));
}

/* 叶子异常类：无自己的成员（继承 Error/Exception 的全部） */
static void ex_register_leaf(const char *name, const char *parent) {
    TClass *cls = ex_new_class(name, CK_CLASS);
    ex_set_parent(cls, parent);
    tphp_class_register_compile(cls);   /* P1-2: 进 master 表，worker 克隆时父类指针可重指 */
}

static _Thread_local bool g_exclass_inited;
void tphp_exclass_init(void) {
    if (g_exclass_inited) return;
    g_exclass_inited = true;

    /* Throwable：标记接口（PHP 中 Throwable 是接口） */
    TClass *thr = ex_new_class("Throwable", CK_INTERFACE);
    tphp_class_register_compile(thr);

    /* Error / Exception：带属性 + 原生 ctor + 四个 getter */
    const char *bases[2] = { "Error", "Exception" };
    for (int i = 0; i < 2; i++) {
        TClass *cls = ex_new_class(bases[i], CK_CLASS);
        ex_add_iface(cls, "Throwable");
        int np = 0;
        TProp *ps = ex_props(&np);
        cls->props = ps;
        cls->n_props = np;
        cls->m_ctor = 0;   /* 下面第一个注册的就是 __construct */
        add_native_method(cls, "__construct", (void *)nm_ex_construct);
        add_native_method(cls, "getMessage", (void *)nm_ex_get_message);
        add_native_method(cls, "getCode", (void *)nm_ex_get_code);
        add_native_method(cls, "getPrevious", (void *)nm_ex_get_previous);
        add_native_method(cls, "getLine", (void *)nm_ex_get_line);
        tphp_class_register_compile(cls);   /* P1-2: 进 master 表，worker 克隆时父类指针可重指 */
    }

    /* Error 家族 */
    ex_register_leaf("TypeError", "Error");
    ex_register_leaf("ArgumentCountError", "Error");
    ex_register_leaf("DivisionByZeroError", "Error");
    ex_register_leaf("UndefinedError", "Error");
    ex_register_leaf("CompileError", "Error");
    ex_register_leaf("DeadlockError", "Error");
    ex_register_leaf("NotCloneableError", "Error");
    ex_register_leaf("UnhandledMatchError", "Error");
    ex_register_leaf("ValueError", "Error");   /* P1-2 补：enum from() 等 */

    /* Exception 家族 */
    ex_register_leaf("RuntimeException", "Exception");
    ex_register_leaf("LogicException", "Exception");
    ex_register_leaf("InvalidArgumentException", "LogicException");
    ex_register_leaf("ErrorException", "Exception");

    /* AsyncCancelled 直承 Throwable（防 catch(Exception) 误吞，既有设计） */
    TClass *ac = ex_new_class("AsyncCancelled", CK_CLASS);
    ex_add_iface(ac, "Throwable");
    int np = 0;
    TProp *ps = ex_props(&np);
    ac->props = ps;
    ac->n_props = np;
    ac->m_ctor = 0;
    add_native_method(ac, "__construct", (void *)nm_ex_construct);
    add_native_method(ac, "getMessage", (void *)nm_ex_get_message);
    add_native_method(ac, "getCode", (void *)nm_ex_get_code);
    add_native_method(ac, "getPrevious", (void *)nm_ex_get_previous);
    add_native_method(ac, "getLine", (void *)nm_ex_get_line);
    tphp_class_register_compile(ac);
}
