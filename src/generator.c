/*
 * TinyPHP.rt - generator.c - PHP 8 generators（帧保存模型）
 * Generator 对象持挂起 VMFrame；next/send/foreach 驱动 resume。
 * v1 范围见 PLAN/generators.md：无 throw()/yield from/dtor-finally。
 */
#include "tphp.h"
#include "value.h"
#include "opcode.h"
#include "class.h"
#include "generator.h"
#include <stdio.h>
#include <string.h>

/* vm.c 辅助（同 tffi 模式） */
void tphp_vm_frame_destroy(VMFrame *f);          /* vm.c: frame_destroy 包装 */
void tphp_vm_run_nested_deep(VM *vm, VMFrame *f); /* vm.c: deep 循环包装 */

/* ---- Generator 状态 ---- */
enum {
    GEN_AT_FIRST = 0,   /* 已创建，未跑（首个 next/rewind/foreach 驱动） */
    GEN_AT_YIELD,       /* 暂停于 yield */
    GEN_RUNNING,
    GEN_CLOSED
};

typedef struct TGen {
    VMFrame *frame;       /* NULL = 已关闭 */
    int state;
    TVal value, key;      /* 最近产出（GEN_CLOSED 后仍可读 current=残留值，
                           * PHP：关闭后 current=null——close 时清为 null） */
    TVal retval;          /* 函数返回值；未返回=T_UNDEF */
    uint32_t send_slot;   /* 最近 yield 的 dst 槽位 */
    bool has_send;
    int64_t auto_key;     /* 自动键计数（初值 -1；PHP：largest_used_integer_key） */
    TVal obj;             /* 回指 Generator 对象（+1，防 dtor 顺序问题） */
} TGen;

#define GEN_MAX 256
static _Thread_local TGen **g_gens;
static _Thread_local int g_n_gens, g_cap_gens;
static _Thread_local TClass *g_gen_cls;
static _Thread_local bool g_gen_inited;

/* vm.c 导出的 deep 嵌套执行 */
void tphp_vm_run_nested_deep(VM *vm, VMFrame *f);

static int gen_reg(TGen *g) {
    if (g_n_gens >= g_cap_gens) {
        g_cap_gens = g_cap_gens ? g_cap_gens * 2 : 16;
        g_gens = tm_realloc(g_gens, sizeof(TGen *) * (size_t)g_cap_gens);
    }
    g_gens[g_n_gens] = g;
    return g_n_gens++;
}

static void add_native_method(TClass *cls, const char *name, void *fn, bool is_static) {
    TMethod *ms = tm_malloc(sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    if (cls->methods)
        memcpy(ms, cls->methods, sizeof(TMethod) * (size_t)cls->n_methods);
    TMethod *m = &ms[cls->n_methods++];
    memset(m, 0, sizeof *m);
    m->name = tstr_intern(name, strlen(name));
    m->visibility = 0;
    m->native = fn;
    m->is_static = is_static;
    tm_free(cls->methods);
    cls->methods = ms;
}

static TGen *gen_of(TObj *o) {
    if (!o || o->cls != g_gen_cls || o->props[0].t != T_INT) return NULL;
    return g_gens[o->props[0].u.i];
}

bool tphp_generator_is_obj(TVal v) {
    return v.t == T_OBJ && ((GCHead *)v.u.p)->kind == GC_KIND_OBJ &&
           gen_of((TObj *)v.u.p) != NULL;
}

/* ---- 生命周期 ---- */

void tphp_generator_dtor(TObj *o) {
    TGen *g = gen_of(o);
    if (!g) return;
    if (g->frame) {
        /* 未耗尽销毁：直接销毁帧（v1 不跑 finally——PLAN/generators.md §6） */
        tphp_vm_frame_destroy(g->frame);
        g->frame = NULL;
    }
    g->state = GEN_CLOSED;
    tval_release(&g->value);
    tval_release(&g->key);
    tval_release(&g->retval);
    tval_release(&g->obj);
    g->value = g->key = g->retval = g->obj = TV_NULL;
}

/* ---- CREATE / YIELD / RETURN（vm.c handler 调用） ---- */

TVal tphp_generator_pack(VM *vm, VMFrame *f) {
    (void)vm;
    TGen *g = tm_malloc(sizeof *g);
    memset(g, 0, sizeof *g);
    g->frame = f;
    g->state = GEN_AT_FIRST;
    g->value = g->key = TV_NULL;
    g->retval = (TVal){ .u.i = 0, .t = T_UNDEF };   /* 未返回标记（memset 会给 T_NULL） */
    g->auto_key = -1;
    g->has_send = false;
    g->send_slot = 0;
    f->gen = g;

    TObj *o = tphp_obj_alloc(g_gen_cls);
    /* props[0] = 注册表下标（int） */
    o->props[0] = TV_INT(gen_reg(g));
    g->obj = TV_OBJ(o);
    tval_addref(g->obj);
    return TV_OBJ(o);   /* +1 交给调用方（DO_FCALL dst） */
}

void tphp_generator_pause(VM *vm, VMFrame *f, TVal v, TVal k, bool has_key,
                          uint32_t send_slot, bool has_send) {
    TGen *g = (TGen *)f->gen;
    if (!g)
        tphp_vm_raise(vm, "Error", "Internal: yield on a non-generator frame");

    tval_addref(v);
    tval_release(&g->value);
    g->value = v;

    /* 键：显式或自动递增（PHP largest_used_integer_key 规则：显式整数键
     * 更新 auto_key 基线，后续无键 yield 从它继续 ++） */
    if (has_key) {
        tval_addref(k);
        if (k.t == T_INT && k.u.i > g->auto_key) g->auto_key = k.u.i;
        tval_release(&g->key);
        g->key = k;
    } else {
        g->auto_key++;
        tval_release(&g->key);
        g->key = TV_INT(g->auto_key);
    }

    g->send_slot = send_slot;
    g->has_send = has_send;
    g->state = GEN_AT_YIELD;
    g->frame = f;

    /* 摘帧：pc 推进到 yield 之后（resume 起点）；控制交还驱动方 */
    f->pc++;
    vm->x.frame = f->prev;
    f->prev = NULL;
}

bool tphp_generator_finish(VM *vm, TVal ret) {
    VMFrame *f = vm->x.frame;
    if (!f->gen) return false;
    TGen *g = (TGen *)f->gen;
    tval_release(&g->retval);
    g->retval = ret;          /* 移交引用（op_return 已 addref） */
    g->state = GEN_CLOSED;
    g->frame = NULL;          /* 本处销毁，dtor 不再碰 */
    tval_release(&g->value); g->value = TV_NULL;
    tval_release(&g->key);   g->key = TV_NULL;
    vm->x.frame = f->prev;
    tphp_vm_frame_destroy(f);
    return true;
}

int tphp_generator_resume(VM *vm, TVal gen, TVal sendv, bool has_send) {
    TObj *o = (TObj *)gen.u.p;
    TGen *g = gen_of(o);
    if (!g) return 0;
    if (g->state == GEN_CLOSED) return 0;          /* next/send 静默（PHP） */
    if (g->state == GEN_RUNNING) {
        /* 重入：帧正在本线程执行（当前帧即 g->frame）。先断开 TGen 与帧
         * （unwind 将销毁该帧——防 dtor/UAF），再抛 PHP 同款错误 */
        g->frame = NULL;
        g->state = GEN_CLOSED;
        tval_release(&g->value); g->value = TV_NULL;
        tval_release(&g->key);   g->key = TV_NULL;
        tphp_vm_raise(vm, "Error", "Cannot resume an already running generator");
    }
    if (!g->frame) { g->state = GEN_CLOSED; return 0; }

    if (has_send && g->has_send) {
        TVal *slot = &g->frame->slots[g->send_slot];
        tval_release(slot);
        *slot = sendv;
        tval_addref(sendv);
    }
    VMFrame *f = g->frame;
    f->prev = vm->x.frame;
    /* 刷新 unwind 归因基线：帧创建于 DO_FCALL，resume 时 n_calls/args 水位
     * 已不同——过期值会让异常弹帧时错乱（RUNNING 重入场景曾 Uncaught） */
    f->n_calls_at_entry = vm->x.n_calls;
    f->args_base = (uint32_t)vm->x.n_args;
    g->state = GEN_RUNNING;
    vm->x.frame = f;
    tphp_vm_run_nested_deep(vm, f);
    return 1;
}

/* ---- foreach ---- */

bool tphp_generator_fe_reset(VM *vm, TVal v) {
    if (!tphp_generator_is_obj(v)) return false;
    TGen *g = gen_of((TObj *)v.u.p);
    if (g->state == GEN_CLOSED)
        tphp_vm_raise(vm, "Error", "Cannot traverse an already closed generator");
    if (g->state == GEN_AT_FIRST)
        tphp_generator_resume(vm, v, TV_NULL, false);   /* 跑到首个 yield */
    return true;
}

/* 协议：reset 已把帧停在首个 yield；fetch 输出当前 key/value 并推进到下
 * 一个 yield。more=false 时 key/val 无效（空生成器或已耗尽）。 */
bool tphp_generator_fe_fetch(VM *vm, TVal gen, TVal *key, TVal *val, bool *more) {
    TGen *g = gen_of((TObj *)gen.u.p);
    if (!g || g->state != GEN_AT_YIELD || !g->frame) {
        *more = false;
        return true;
    }
    *val = g->value;  tval_addref(*val);
    *key = g->key;    tval_addref(*key);
    tphp_generator_resume(vm, gen, TV_NULL, false);   /* 消费后推进 */
    *more = true;
    return true;
}

/* ---- native 方法 ---- */

static TVal nm_gen_rewind(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    TGen *g = gen_of(o);
    if (!g) tphp_vm_raise(vm, "Error", "Generator::rewind(): invalid object");
    if (g->state == GEN_AT_FIRST) {
        TVal sv = TV_OBJ(o);
        tphp_generator_resume(vm, sv, TV_NULL, false);
        return TV_NULL;
    }
    /* PHP：已跑过（含已关闭）→ Error */
    tphp_vm_raise(vm, "Error", "Cannot rewind a generator that was already run");
    return TV_NULL;
}

static TVal nm_gen_valid(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TGen *g = gen_of((TObj *)self);
    return (g && g->state != GEN_CLOSED && g->frame) ? TV_TRUE : TV_FALSE;
}

static TVal nm_gen_current(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    TGen *g = gen_of(o);
    if (!g) return TV_NULL;
    if (g->state == GEN_AT_FIRST)   /* PHP：current/key 也 ensure_initialized */
        tphp_generator_resume(vm, TV_OBJ(o), TV_NULL, false);
    if (g->state == GEN_CLOSED) return TV_NULL;
    TVal v = g->value;
    tval_addref(v);
    return v;
}

static TVal nm_gen_key(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    TGen *g = gen_of(o);
    if (!g) return TV_NULL;
    if (g->state == GEN_AT_FIRST)
        tphp_generator_resume(vm, TV_OBJ(o), TV_NULL, false);
    if (g->state == GEN_CLOSED) return TV_NULL;
    TVal k = g->key;
    tval_addref(k);
    return k;
}

static TVal nm_gen_next(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    TVal sv = TV_OBJ(o);
    TGen *g = gen_of(o);
    if (g && g->state == GEN_AT_FIRST)
        tphp_generator_resume(vm, sv, TV_NULL, false);   /* ensure_initialized */
    tphp_generator_resume(vm, sv, TV_NULL, false);       /* 推进 */
    return TV_NULL;
}

static TVal nm_gen_send(VM *vm, void *self, TVal *args, int n) {
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError", "Generator::send() expects 1 argument");
    TObj *o = (TObj *)self;
    TVal sv = TV_OBJ(o);
    TGen *g = gen_of(o);
    if (g && g->state == GEN_AT_FIRST)
        tphp_generator_resume(vm, sv, TV_NULL, false);   /* ensure_initialized */
    tphp_generator_resume(vm, sv, args[0], true);        /* 写 send + 推进 */
    if (!g || g->state == GEN_CLOSED) return TV_NULL;
    TVal v = g->value;
    tval_addref(v);
    return v;
}

static TVal nm_gen_getreturn(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TGen *g = gen_of((TObj *)self);
    if (!g || g->retval.t == T_UNDEF)
        tphp_vm_raise(vm, "Error",
                      "Cannot get return value of a generator that hasn't returned");
    TVal v = g->retval;
    tval_addref(v);
    return v;
}

/* ---- 类注册 ---- */

void tphp_generator_init(void) {
    if (g_gen_inited) return;
    g_gen_inited = true;
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern("Generator", 9);
    cls->kind = CK_CLASS;
    cls->flattened = true;
    cls->n_props = 1;
    cls->props = tm_malloc(sizeof(TProp));
    memset(cls->props, 0, sizeof(TProp));
    cls->props[0].name = tstr_intern("__handle", 8);
    cls->props[0].slot = 0;
    cls->m_ctor = cls->m_dtor = cls->m_get = cls->m_set = cls->m_isset =
        cls->m_unset = cls->m_call = cls->m_callstatic = cls->m_tostring =
        cls->m_invoke = cls->m_clone = cls->m_debuginfo = cls->m_clonestate = -1;
    add_native_method(cls, "rewind", (void *)nm_gen_rewind, false);
    add_native_method(cls, "valid", (void *)nm_gen_valid, false);
    add_native_method(cls, "current", (void *)nm_gen_current, false);
    add_native_method(cls, "key", (void *)nm_gen_key, false);
    add_native_method(cls, "next", (void *)nm_gen_next, false);
    add_native_method(cls, "send", (void *)nm_gen_send, false);
    add_native_method(cls, "getReturn", (void *)nm_gen_getreturn, false);
    cls->native_dtor = tphp_generator_dtor;
    tphp_class_register(cls);
    g_gen_cls = cls;
}
