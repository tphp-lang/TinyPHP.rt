/*
 * TinyPHP.rt - worker.c - M5-c 真 OS 线程 Worker（txiki 模式）
 *
 * 模型（PLAN/02 §3 + PLAN/03 §5）：
 *   - 子线程 = 独立 runtime（thread-local VM/loop/调度器/类表克隆/堆），
 *     与父线程绝不共享 PHP 对象；并发安全靠隔离 + 消息
 *   - mailbox：uv_mutex + 消息链表 + uv_async_send 唤醒对端 loop；
 *     消息体 = serialize 字节流（serialize.c 协议），收端 unserialize
 *   - 入口闭包：fn 以 prog 内索引共享（freeze 后字节码只读），捕获值
 *     逐个 serialize 进子线程
 *   - 生命周期父线程独占：post/recv/on；stop → 关闭子 mailbox（EOF）→
 *     子线程入口返回 → 父 join
 *
 * PHP API：
 *   $w = new tphp\Worker(function () { $me = Worker::self(); ... });
 *   $w->post($v); $w->recv(); $w->on('message'|'close', fn); $w->stop();
 *   $w->isRunning(); $w->id();
 *   子线程内：$me = tphp\Worker::self(); $me->recv(); $me->post($r);
 */
#include "tphp.h"
#include "value.h"
#include "opcode.h"
#include "class.h"
#include "sched.h"
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>   /* M6.2: portable atomics (replaces Win32 Interlocked*) */

/* ---- serialize.c / vm.c ---- */
TStr *tphp_serialize_val(struct VM *vm, TVal v);
bool  tphp_unserialize_val(struct VM *vm, const char *d, size_t n, TVal *out);
TStr *tstr_new(const char *s, size_t len);
void  tphp_vm_raise(struct VM *vm, const char *cls, const char *fmt, ...);
TVal  tphp_vm_call_callable(struct VM *vm, TVal callable, TVal *args, int n);
int   tphp_vm_run_closure(Program *prog, TClosure *clo);
void  tphp_worker_init(void);            /* 本文件尾：内部类注册 */
void  tphp_async_init(void);             /* sched.c */
void  tphp_io_init(void);                /* io.c */
void  tphp_ffi_init(void);               /* ffi.c */
/* mem.c */
void  tm_free(void *p);
void  gc_register(GCHead *h);
/* sched.c */
struct uv_loop_s *tphp_sched_loop(void);

/* ---- mailbox：单向消息队列 ---- */

typedef struct WMsg {
    struct WMsg *next;
    char *data;
    uint32_t len;
} WMsg;

typedef struct WBox {
    uv_mutex_t mu;
    WMsg *head, *tail;
    uv_async_t async;          /* 接收方 loop 的唤醒器 */
    bool async_inited;
    SchedEvent *rx_ev;         /* 仅接收线程读写 */
    bool eof;                  /* 对端已关闭 */
} WBox;

static void wbox_init(WBox *b) {
    uv_mutex_init(&b->mu);
    b->head = b->tail = NULL;
    b->async_inited = false;
    b->rx_ev = NULL;
    b->eof = false;
}

/* async 回调（接收线程 loop）：fire 挂起中的 recv */
static void wbox_async_cb(uv_async_t *a);

/* 接收方 loop 里初始化 async（必须在接收线程调用） */
static void wbox_bind_rx(WBox *b, uv_loop_t *loop) {
    uv_async_init(loop, &b->async, wbox_async_cb);
    b->async.data = b;
    b->async_inited = true;
}

/* 发送方：入队 + 唤醒接收方（data 所有权转移；data=NULL = EOF 控制） */
static void wbox_put(WBox *b, char *data, uint32_t len) {
    WMsg *m = tm_malloc(sizeof *m);
    m->next = NULL;
    m->data = data;
    m->len = len;
    uv_mutex_lock(&b->mu);
    if (b->tail) b->tail->next = m;
    else b->head = m;
    b->tail = m;
    uv_mutex_unlock(&b->mu);
    if (b->async_inited) uv_async_send(&b->async);
}

/* 对端关闭：置 EOF + 唤醒 */
static void wbox_close(WBox *b) {
    uv_mutex_lock(&b->mu);
    b->eof = true;
    uv_mutex_unlock(&b->mu);
    if (b->async_inited) uv_async_send(&b->async);
}

/* 接收方：取一条消息（data 所有权转移；NULL = 空）
 * *eof_out 返回队列空且对端已关闭 */
static char *wbox_take(WBox *b, uint32_t *len_out, bool *eof_out) {
    char *data = NULL;
    uv_mutex_lock(&b->mu);
    WMsg *m = b->head;
    if (m) {
        b->head = m->next;
        if (!b->head) b->tail = NULL;
        data = m->data;
        *len_out = m->len;
        tm_free(m);
    } else {
        *eof_out = b->eof;
    }
    uv_mutex_unlock(&b->mu);
    return data;
}

/* async 回调（接收线程 loop）：fire 挂起中的 recv */
static void wbox_async_cb(uv_async_t *a) {
    WBox *b = (WBox *)a->data;
    if (b->rx_ev) {
        SchedEvent *ev = b->rx_ev;
        b->rx_ev = NULL;
        tphp_sched_event_fire(ev);
    }
}

/* 接收方挂起等消息：返回 data（转移）或 NULL（EOF）。
 * 用标准事件三件套（waiter 登记 → async cb fire 生效）；ev 在协程栈上
 * ——挂起期间栈冻结，async cb 跨 fiber 写安全。event_wait 与 async cb
 * 同线程（cb 只在本协程挂起后跑），无竞态窗口。 */
static char *wbox_recv(WBox *b, uint32_t *len_out) {
    for (;;) {
        bool eof = false;
        char *data = wbox_take(b, len_out, &eof);
        if (data) return data;
        if (eof) return NULL;
        SchedEvent ev;
        tphp_sched_event_reset(&ev);
        b->rx_ev = &ev;          /* 仅本线程写；async cb 也在本线程 */
        tphp_sched_event_wait(&ev);
        b->rx_ev = NULL;
    }
}

/* ---- WHandle（进程级，跨线程共享） ---- */

typedef struct WHandle {
    int id;
    uv_thread_t tid;
    Program *prog;               /* 共享只读（freeze 后） */
    WBox to_child, to_parent;
    uv_sem_t ready;              /* 子线程 mailbox 就绪握手（PLAN/03 §5） */
    char *entry_blob;            /* 入口闭包序列化 */
    uint32_t entry_len;
    atomic_int state;          /* 0 running / 1 stop_requested / 2 done */
    /* 父侧事件回调（仅父线程读写） */
    TVal on_message;             /* T_NULL = 无 */
} WHandle;

static WHandle **g_handles;
static int g_n_handles, g_cap_handles;
static uv_mutex_t g_handles_mu;
static bool g_handles_mu_inited;

static _Thread_local WHandle *g_self_handle;   /* 子线程视角 */
static _Thread_local TClass *g_worker_cls;
static _Thread_local TClass *g_wctx_cls;
static _Thread_local bool g_worker_inited;

static int handle_register(WHandle *h) {
    if (!g_handles_mu_inited) {
        uv_mutex_init(&g_handles_mu);
        g_handles_mu_inited = true;
    }
    uv_mutex_lock(&g_handles_mu);
    if (g_n_handles == g_cap_handles) {
        g_cap_handles = g_cap_handles ? g_cap_handles * 2 : 8;
        g_handles = tm_realloc(g_handles, sizeof(WHandle *) * g_cap_handles);
    }
    h->id = g_n_handles;
    g_handles[g_n_handles++] = h;
    uv_mutex_unlock(&g_handles_mu);
    return h->id;
}

static WHandle *handle_get(int64_t id) {
    if (!g_handles_mu_inited) return NULL;
    uv_mutex_lock(&g_handles_mu);
    WHandle *h = (id >= 0 && id < g_n_handles) ? g_handles[id] : NULL;
    uv_mutex_unlock(&g_handles_mu);
    return h;
}

/* ---- 入口闭包序列化：'W' + u32 fn_idx + u32 n_cap + n × (u32 len + val 流) ---- */

typedef struct WBuf { char *buf; size_t len, cap; } WBuf;

static void wbuf_put(WBuf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 64;
        while (nc < b->len + n) nc *= 2;
        b->buf = tm_realloc(b->buf, nc);
        b->cap = nc;
    }
    memcpy(b->buf + b->len, d, n);
    b->len += n;
}

static uint32_t prog_func_index(Program *p, TFunc *fn) {
    for (int i = 0; i < p->n_funcs; i++)
        if (p->funcs[i] == fn) return (uint32_t)i;
    return UINT32_MAX;
}

static char *worker_serialize_closure(Program *p, TClosure *clo,
                                      uint32_t *out_len) {
    uint32_t fi = prog_func_index(p, clo->fn);
    if (fi == UINT32_MAX || clo->bfn) return NULL;
    WBuf b = { NULL, 0, 0 };
    wbuf_put(&b, "W", 1);
    wbuf_put(&b, &fi, 4);
    uint32_t nc = clo->n_cap;
    wbuf_put(&b, &nc, 4);
    for (uint16_t i = 0; i < clo->n_cap; i++) {
        TStr *s = tphp_serialize_val(NULL, clo->captures[i]);
        uint32_t sl = (uint32_t)s->len;
        wbuf_put(&b, &sl, 4);
        wbuf_put(&b, tstr_val(s), s->len);
        tval_release(&(TVal){ .u.p = s, .t = T_STR });
    }
    *out_len = (uint32_t)b.len;
    return b.buf;
}

/* 子线程侧：从 blob 重建入口闭包（本线程堆；fn 指针共享只读） */
static TClosure *worker_unserialize_closure(Program *p, const char *d,
                                            size_t n, size_t *pos) {
    if (*pos + 1 > n || d[*pos] != 'W') return NULL;
    (*pos)++;
    uint32_t fi, nc;
    if (*pos + 8 > n) return NULL;
    memcpy(&fi, d + *pos, 4); *pos += 4;
    memcpy(&nc, d + *pos, 4); *pos += 4;
    if (fi >= (uint32_t)p->n_funcs || nc > 256) return NULL;
    TFunc *fn = p->funcs[fi];
    TVal *caps = tm_malloc(sizeof(TVal) * (size_t)(nc ? nc : 1));
    for (uint32_t i = 0; i < nc; i++) {
        if (*pos + 4 > n) { tm_free(caps); return NULL; }
        uint32_t sl;
        memcpy(&sl, d + *pos, 4); *pos += 4;
        if (*pos + sl > n) { tm_free(caps); return NULL; }
        TVal v;
        if (!tphp_unserialize_val(NULL, d + *pos, sl, &v)) {
            tm_free(caps);
            return NULL;
        }
        *pos += sl;
        caps[i] = v;
    }
    /* build the closure (rc=1, owned by the root frame's capture slots) */
    TClosure *clo = tm_malloc(sizeof *clo);
    clo->h.rc = 1;
    clo->h.color = 0;
    clo->h.kind = GC_KIND_CLO;
    clo->h.flags = 0;
    extern void gc_register(GCHead *h);
    gc_register(&clo->h);
    clo->fn = fn;
    clo->bfn = NULL;
    clo->n_cap = (uint16_t)nc;
    clo->captures = caps;        /* 转移 */
    return clo;
}

/* ---- 子线程主函数 ---- */

static void worker_thread(void *arg) {
    WHandle *h = (WHandle *)arg;
    g_self_handle = h;

    /* 独立 runtime 初始化（thread-local 全套） */
    tphp_worker_init();          /* 内部类（本线程） */
    tphp_async_init();
    tphp_ffi_init();             /* M5.5：Worker 线程内也可用 FFI */
    tphp_io_init();
    tphp_class_tab_clone_thread();
    tphp_sched_loop_init();      /* async bind 前 loop 必须已初始化 */

    /* mailbox 的接收侧绑到本线程 loop；完成后通知父线程（sem 握手：
     * ctor 返回时子线程 mailbox 已就绪，此后 post 的 async 必达） */
    wbox_bind_rx(&h->to_child, tphp_sched_loop());
    uv_sem_post(&h->ready);

    /* 入口闭包重建 + 以它为 main 跑独立调度器 */
    size_t pos = 0;
    TClosure *clo = worker_unserialize_closure(h->prog, h->entry_blob,
                                               h->entry_len, &pos);
    if (!clo) {
        fprintf(stderr, "PHP Warning: Worker: failed to rebuild entry\n");
        atomic_store(&h->state, 2);
        wbox_close(&h->to_parent);
        return;
    }
    tm_free(h->entry_blob);
    h->entry_blob = NULL;

    tphp_vm_run_closure(h->prog, clo);

    atomic_store(&h->state, 2);
    wbox_close(&h->to_parent);   /* 通知父线程：已退出 */
}

/* ---- 内部类 native 方法 ---- */

/* Worker（父侧）/ WorkerContext（子侧）共用的对象 → handle */
static WHandle *worker_obj_handle(void *selfv) {
    TObj *o = (TObj *)selfv;
    if (!o || o->props[0].t != T_INT) return NULL;
    return handle_get(o->props[0].u.i);
}

/* new Worker(entry, opts = []) */
static TVal nm_worker_construct(VM *vm, void *self, TVal *args, int n) {
    (void)self;
    TObj *o = (TObj *)self;
    if (n < 1 || (args[0].t != T_CLOSURE && args[0].t != T_STR))
        tphp_vm_raise(vm, "TypeError",
                      "Worker::__construct() expects a Closure or string");
    if (args[0].t != T_CLOSURE)
        tphp_vm_raise(vm, "TypeError",
                      "Worker: string entry is not supported yet (use a Closure)");
    Program *prog = vm->prog;
    uint32_t blen;
    char *blob = worker_serialize_closure(prog, (TClosure *)args[0].u.p, &blen);
    if (!blob)
        tphp_vm_raise(vm, "TypeError",
                      "Worker: entry closure cannot be shared (first-class "
                      "builtin wrappers are not supported)");
    WHandle *h = tm_malloc(sizeof *h);
    memset(h, 0, sizeof *h);
    h->prog = prog;
    h->entry_blob = blob;
    h->entry_len = blen;
    wbox_init(&h->to_child);
    wbox_init(&h->to_parent);
    uv_sem_init(&h->ready, 0);
    handle_register(h);
    o->props[0] = TV_INT(h->id);

    wbox_bind_rx(&h->to_parent, tphp_sched_loop());  /* 父接收侧 */
    uv_thread_create(&h->tid, worker_thread, h);
    uv_sem_wait(&h->ready);       /* 等子线程 mailbox 就绪（PLAN/03 §5） */
    return TV_NULL;
}

/* post($value)：serialize → 对向 mailbox（父→子 / 子→父 按视角） */
static TVal nm_worker_post(VM *vm, void *self, TVal *args, int n) {
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "post() expects at least 1 argument");
    WHandle *h = worker_obj_handle(self);
    if (!h) return TV_FALSE;
    bool child_side = (g_self_handle == h);
    TStr *s = tphp_serialize_val(vm, args[0]);
    char *data = tm_malloc(s->len);
    memcpy(data, tstr_val(s), s->len);
    uint32_t len = (uint32_t)s->len;
    tval_release(&(TVal){ .u.p = s, .t = T_STR });
    wbox_put(child_side ? &h->to_parent : &h->to_child, data, len);
    return TV_NULL;
}

/* recv()：对向 mailbox 取一条（挂起）；EOF 返回 false */
static TVal nm_worker_recv(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    WHandle *h = worker_obj_handle(self);
    if (!h) return TV_FALSE;
    bool child_side = (g_self_handle == h);
    WBox *box = child_side ? &h->to_child : &h->to_parent;
    uint32_t len;
    char *data = wbox_recv(box, &len);
    if (!data) return TV_FALSE;          /* 对端关闭 */
    TVal v;
    if (!tphp_unserialize_val(vm, data, len, &v)) {
        tm_free(data);
        fprintf(stderr, "PHP Warning: Worker recv(): malformed message\n");
        return TV_FALSE;
    }
    tm_free(data);
    return v;
}

/* on('message'|'close', callable)：父侧注册（首版只支持 message，且由
 * stop() 前的 drain 语义配合；正式分发见 M5-d 验收脚本用 recv） */
static TVal nm_worker_on(VM *vm, void *self, TVal *args, int n) {
    if (n < 2 || args[0].t != T_STR)
        tphp_vm_raise(vm, "TypeError", "on() expects (string, callable)");
    WHandle *h = worker_obj_handle(self);
    if (!h) return TV_NULL;
    if (tstr_case_eq(tstr_val((TStr *)args[0].u.p), "message")) {
        if (h->on_message.t != T_NULL) tval_release(&h->on_message);
        h->on_message = args[1];
        tval_addref(h->on_message);
    } else {
        fprintf(stderr, "PHP Warning: Worker::on(): event \"%s\" ignored\n",
                tstr_val((TStr *)args[0].u.p));
    }
    return TV_NULL;
}

/* stop()：关子侧 mailbox（EOF）→ 子线程入口自然退出 → join。
 * 排空期间分发 on('message') 回调（PLAN §6 的 on 语义：消息在 drain
 * 时到达父线程并触发回调）。 */
static TVal nm_worker_stop(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    WHandle *h = worker_obj_handle(self);
    if (!h) return TV_NULL;
    if (atomic_exchange(&h->state, 1) == 2) return TV_NULL;  /* 已 done */
    wbox_close(&h->to_child);
    uint32_t len;
    char *data;
    while ((data = wbox_recv(&h->to_parent, &len)) != NULL) {
        TVal msg;
        if (tphp_unserialize_val(vm, data, len, &msg)) {
            if (h->on_message.t != T_NULL)
                tphp_vm_call_callable(vm, h->on_message, &msg, 1);
            tval_release(&msg);
        }
        tm_free(data);
    }
    uv_thread_join(&h->tid);
    return TV_NULL;
}

static TVal nm_worker_is_running(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    WHandle *h = worker_obj_handle(self);
    return (h && h->state != 2) ? TV_TRUE : TV_FALSE;
}

static TVal nm_worker_id(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    WHandle *h = worker_obj_handle(self);
    return TV_INT(h ? h->id : -1);
}

/* static Worker::self()：子线程内取自身上下文（WorkerContext） */
static TVal nm_worker_self(VM *vm, TVal *args, int n) {
    (void)args; (void)n;
    if (!g_self_handle || !g_wctx_cls)
        tphp_vm_raise(vm, "Error", "Worker::self() outside a worker thread");
    TObj *o = tphp_obj_alloc(g_wctx_cls);
    o->props[0] = TV_INT(g_self_handle->id);
    return TV_OBJ(o);
}

/* ---- 类注册 ---- */

static void worker_add_native(TClass *cls, const char *name, void *native,
                              bool is_static) {
    TMethod *ms = tm_malloc(sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    if (cls->methods) {
        memcpy(ms, cls->methods, sizeof(TMethod) * (size_t)cls->n_methods);
        tm_free(cls->methods);
    }
    TMethod *m = &ms[cls->n_methods++];
    memset(m, 0, sizeof *m);
    m->name = tstr_intern(name, strlen(name));
    m->visibility = 0;            /* CVIS_PUBLIC */
    m->native = native;
    m->is_static = is_static;
    cls->methods = ms;
}

static TClass *worker_make_class(const char *name) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern(name, strlen(name));
    cls->kind = 0;                /* CK_CLASS */
    cls->flattened = true;
    cls->m_ctor = -1; cls->m_dtor = -1; cls->m_get = -1; cls->m_set = -1;
    cls->m_isset = -1; cls->m_unset = -1; cls->m_call = -1;
    cls->m_callstatic = -1; cls->m_tostring = -1; cls->m_invoke = -1;
    cls->m_clone = -1; cls->m_debuginfo = -1; cls->m_clonestate = -1;
    /* __handle：WHandle 注册表下标 */
    TProp *p = tm_malloc(sizeof(TProp));
    memset(p, 0, sizeof *p);
    p->name = tstr_intern("__handle", 8);
    p->defval = TV_NULL;
    p->slot = 0;
    cls->props = p;
    cls->n_props = 1;
    return cls;
}

/* 内部类注册（编译前调用；每线程独立 TClass，注册进本线程表） */
void tphp_worker_init(void) {
    if (g_worker_inited) return;
    g_worker_inited = true;

    TClass *w = worker_make_class("tphp\\Worker");
    w->m_ctor = (int16_t)w->n_methods;
    worker_add_native(w, "__construct", (void *)nm_worker_construct, false);
    worker_add_native(w, "post", (void *)nm_worker_post, false);
    worker_add_native(w, "recv", (void *)nm_worker_recv, false);
    worker_add_native(w, "on", (void *)nm_worker_on, false);
    worker_add_native(w, "stop", (void *)nm_worker_stop, false);
    worker_add_native(w, "isrunning", (void *)nm_worker_is_running, false);
    worker_add_native(w, "id", (void *)nm_worker_id, false);
    worker_add_native(w, "self", (void *)nm_worker_self, true);
    tphp_class_register(w);
    g_worker_cls = w;

    TClass *c = worker_make_class("tphp\\WorkerContext");
    worker_add_native(c, "post", (void *)nm_worker_post, false);
    worker_add_native(c, "recv", (void *)nm_worker_recv, false);
    worker_add_native(c, "isrunning", (void *)nm_worker_is_running, false);
    worker_add_native(c, "id", (void *)nm_worker_id, false);
    tphp_class_register(c);
    g_wctx_cls = c;
}
