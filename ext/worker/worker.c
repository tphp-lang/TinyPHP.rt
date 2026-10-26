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
#include "tsched.h"
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>   /* M6.2: portable atomics (replaces Win32 Interlocked*) */

/* ---- serialize.c / vm.c ---- */
TStr *tphp_serialize_val(struct VM *vm, TVal v);
bool  tphp_unserialize_val(struct VM *vm, const char *d, size_t n, TVal *out);
TStr *tstr_new(const char *s, size_t len);
void  tphp_vm_raise(struct VM *vm, const char *cls, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
TVal  tphp_vm_call_callable(struct VM *vm, TVal callable, TVal *args, int n);
int   tphp_vm_run_closure(Program *prog, TClosure *clo, TVal *main_ret);
Program *tphp_vm_find_func_owner(TFunc *fn);   /* vm.c：函数所属 Program */
void  tphp_prog_freeze(Program *p);            /* vm.c：幂等（flatten+IC 预解析） */
void  tphp_worker_init(void);            /* 本文件尾：内部类注册 */
void  tphp_async_init(void);             /* sched.c */
void  tphp_io_init(void);                /* io.c */
void  tphp_ffi_init(void);               /* ffi.c */
void  tphp_exclass_init(void);           /* exclass.c P1-2 */
void  tphp_closure_class_init(void);     /* vm.c P1-3 */
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

/* 入口闭包序列化：自动定位所属 Program（主 或 inc_progs）。
 * inc_progs 未 freeze（IC 运行期写）→ 共享前对 owner 执行 freeze
 * （幂等：flatten 跳过已完成，IC 只解析空位 → 此后字节码只读）。
 * 这解锁了 vendor/库文件里的闭包也能作为 Worker 入口。 */
static char *worker_serialize_closure(VM *vm, TClosure *clo,
                                      uint32_t *out_len, Program **owner_out) {
    Program *owner = tphp_vm_find_func_owner(clo->fn);
    if (!owner || clo->bfn) return NULL;
    if (owner != vm->prog) tphp_prog_freeze(owner);
    uint32_t fi = prog_func_index(owner, clo->fn);
    if (fi == UINT32_MAX) return NULL;
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
    if (owner_out) *owner_out = owner;
    return b.buf;
}

/* 顶层函数入口：与闭包同格式但无捕获（nc = 0）。
 * 用于 new Worker("fn_name")——函数必须是主 Program 的顶层函数。 */
static char *worker_serialize_func(Program *p, TFunc *fn, uint32_t *out_len) {
    uint32_t fi = prog_func_index(p, fn);
    if (fi == UINT32_MAX) return NULL;
    WBuf b = { NULL, 0, 0 };
    wbuf_put(&b, "W", 1);
    wbuf_put(&b, &fi, 4);
    uint32_t nc = 0;
    wbuf_put(&b, &nc, 4);
    *out_len = (uint32_t)b.len;
    return b.buf;
}

/* 子线程侧：从 blob 重建入口闭包（本线程堆；fn 指针共享只读） */static TClosure *worker_unserialize_closure(Program *p, const char *d,
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
    tphp_closure_class_init();   /* P1-3（线程表注册，同 Coroutine 模式；
                                  * exclass 的异常类经 clone_thread 从 master
                                  * 克隆，此处不可重复注册） */
    tphp_class_tab_clone_thread();
    tphp_sched_loop_init();      /* async bind 前 loop 必须已初始化 */

    /* mailbox 的接收侧绑到本线程 loop；完成后通知父线程（sem 握手：
     * ctor 返回时子线程 mailbox 已就绪，此后 post 的 async 必达） */
    wbox_bind_rx(&h->to_child, tphp_sched_loop());
    uv_sem_post(&h->ready);

    /* 入口闭包重建 + 以它为 main 跑独立调度器；main 的返回值回传父线程 */
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

    TVal ret = TV_NULL;
    tphp_vm_run_closure(h->prog, clo, &ret);
    TStr *rs = tphp_serialize_val(NULL, ret);
    tval_release(&ret);              /* main 返回值的引用归本线程持有 */
    char *rdata = tm_malloc(rs->len);
    memcpy(rdata, tstr_val(rs), rs->len);
    wbox_put(&h->to_parent, rdata, (uint32_t)rs->len);
    tval_release(&(TVal){ .u.p = rs, .t = T_STR });

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
    Program *prog = vm->prog;
    Program *owner = prog;               /* 入口闭包所属 Program（跨线程共享给子线程） */
    uint32_t blen = 0;
    char *blob = NULL;
    if (args[0].t == T_CLOSURE) {
        blob = worker_serialize_closure(vm, (TClosure *)args[0].u.p, &blen, &owner);
        if (!blob)
            tphp_vm_raise(vm, "TypeError",
                          "Worker: entry closure cannot be shared (first-class "
                          "builtin wrappers are not supported)");
    } else {
        /* 字符串入口 = 主 Program 里的顶层函数名（无参数） */
        TStr *name = (TStr *)args[0].u.p;
        TFunc *fn = NULL;
        for (int i = 0; i < prog->n_funcs; i++) {
            TFunc *f = prog->funcs[i];
            if (f->name && !f->is_closure && tstr_eq(f->name, name)) {
                fn = f;
                break;
            }
        }
        if (!fn)
            tphp_vm_raise(vm, "Error",
                          "Worker: entry function %s() is not defined "
                          "(must be a top-level function in the main script)",
                          tstr_val(name));
        blob = worker_serialize_func(prog, fn, &blen);
        if (!blob)
            tphp_vm_raise(vm, "TypeError",
                          "Worker: entry function cannot be shared");
    }
    WHandle *h = tm_malloc(sizeof *h);
    memset(h, 0, sizeof *h);
    h->prog = owner;      /* 入口闭包所属 Program（可能来自 inc_progs，已 freeze） */
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

/* tryRecv()：非阻塞取一条消息。有 → 值；无 → null；对端关闭 → false。
 * 与 recv() 的唯一区别是不挂起协程——UI 线程（如 webview 的 bind 回调）
 * 可以安全调用它收割后台结果，不会冻结事件循环。 */
static TVal nm_worker_try_recv(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    WHandle *h = worker_obj_handle(self);
    if (!h) return TV_FALSE;
    bool child_side = (g_self_handle == h);
    WBox *box = child_side ? &h->to_child : &h->to_parent;
    uint32_t len = 0;
    bool eof = false;
    char *data = wbox_take(box, &len, &eof);
    if (!data) return eof ? TV_FALSE : TV_NULL;
    TVal v;
    if (!tphp_unserialize_val(vm, data, len, &v)) {
        tm_free(data);
        fprintf(stderr, "PHP Warning: Worker tryRecv(): malformed message\n");
        return TV_NULL;
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
    if (atomic_exchange(&h->state, 1) == 2) {
        /* 子线程已自行结束（state=done）：仍要 join 回收 OS 线程资源
         * （uv_thread_t + 线程栈），否则泄漏。早前这里直接 return，
         * 把"已完成"和"未 join"混成了一件事。 */
        uv_thread_join(&h->tid);
        return TV_NULL;
    }
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

/* ---- Tphp\Thread（真线程池）----
 * spawn(callable)：起一个一次性 Worker，入口就是该 callable（在主脚本
 *   编译，字节码跨线程共享）。callable 执行完线程自然退出，返回值经
 *   to_parent 发回。
 * wait()：按 spawn 顺序收集结果（阻塞等待，不能在 bind 回调里用）。
 * detach()：不再跟踪——线程自己跑完，WHandle 由进程退出回收。
 * 注意：这是真并行（OS 线程），不是协程；在 bind 回调里 spawn 是安全的
 * （不依赖 main 挂起），但不能在子线程里再调 Tphp\Worker（类已内部化）。 */
static _Thread_local TClass *g_thread_cls;

/* detach 的 Worker 交给全局孤儿表：线程自己跑完，进程退出时统一回收。
 * 不能在这里 release——Worker 线程可能还在访问其 props/闭包，提前 free
 * 会在 mimalloc 退出校验时报堆损坏（实测 C 回调 + detach 场景）。 */
static _Thread_local TArr *g_thread_orphans;

static TVal nm_thread_construct(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    o->props[0] = TV_ARR(tarr_new());
    return TV_NULL;
}

static TVal nm_thread_spawn(VM *vm, void *self, TVal *args, int n) {
    TObj *t = (TObj *)self;
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Thread::spawn expects a callable");
    /* 一次性 Worker：入口 = args[0]（主 Program 里的 thunk 闭包） */
    TObj *w = tphp_obj_alloc(g_worker_cls);
    nm_worker_construct(vm, w, args, n);
    TArr *a = (TArr *)t->props[0].u.p;
    /* 引用计数：tarr_append 不做 addref（调用方自己加，见 vm.c op_assign），
     * 所以数组要自己持有一份 w 的引用。返回给调用方的 spawn() 结果是
     * 另一份引用——两份都必须独立计数：
     *   tphp_obj_alloc → rc=1（本次转移给返回值）
     *   tval_addref    → rc=2（数组持有的那一份）
     * 早前只 addref 一次，把同一份引用同时给了数组和返回值：数组释放时
     * 提前 free(w)，调用方 frame 随后再 release → 二次释放 → UAF
     * （mimalloc 报 "corrupted free list entry"）。 */
    tval_addref(TV_OBJ(w));
    tarr_append(a, TV_OBJ(w));
    return TV_OBJ(w);
}

static TVal nm_thread_wait(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *t = (TObj *)self;
    TArr *a = (TArr *)t->props[0].u.p;
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        TVal wv = a->data[i];
        if (wv.t != T_OBJ) { tarr_append(out, TV_NULL); continue; }
        TVal r = nm_worker_recv(vm, wv.u.p, NULL, 0);   /* 阻塞等结果 */
        tarr_append(out, r);
        nm_worker_stop(vm, wv.u.p, NULL, 0);            /* join + 关闭 */
    }
    TVal old = t->props[0];
    t->props[0] = TV_ARR(tarr_new());
    tval_release(&old);
    return TV_ARR(out);
}

static TVal nm_thread_detach(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *t = (TObj *)self;
    TArr *a = (TArr *)t->props[0].u.p;
    if (!g_thread_orphans) g_thread_orphans = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        if (a->data[i].t == T_OBJ) {
            tarr_append(g_thread_orphans, a->data[i]);   /* 引用移交孤儿表 */
            a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
        }
    }
    TVal old = t->props[0];
    t->props[0] = TV_ARR(tarr_new());
    tval_release(&old);    /* 现在只剩空数组，释放安全 */
    return TV_NULL;
}

static TVal nm_thread_count(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return TV_INT(((TArr *)((TObj *)self)->props[0].u.p)->n);
}

/* poll()：非阻塞收割已完成 Worker 的返回值（main_ret 经 to_parent 回传）。
 * 返回结果数组（按完成序）；已完成的 Worker 同步 stop 并移出跟踪。
 * 与 wait() 的区别：不挂起协程——UI 线程（bind 回调）可安全调用。 */
static TVal nm_thread_poll(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *t = (TObj *)self;
    TArr *a = (TArr *)t->props[0].u.p;
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < a->dused; i++) {
        TVal wv = a->data[i];
        if (wv.t != T_OBJ) continue;
        TVal m = nm_worker_try_recv(vm, wv.u.p, NULL, 0);
        if (m.t == T_NULL || m.t == T_FALSE) continue;   /* 未完成/已关闭 */
        tarr_append(out, m);
        nm_worker_stop(vm, wv.u.p, NULL, 0);             /* 已完成：join */
        /* 移出跟踪：把数组持有的那一份引用释放掉（不能只置 UNDEF——
         * 那是泄漏；也不能提前释放——worker 还在用它）。 */
        tval_release(&wv);
        a->data[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    return TV_ARR(out);
}


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

    TClass *w = worker_make_class("Tphp\\Worker");
    w->m_ctor = (int16_t)w->n_methods;
    worker_add_native(w, "__construct", (void *)nm_worker_construct, false);
    worker_add_native(w, "post", (void *)nm_worker_post, false);
    worker_add_native(w, "recv", (void *)nm_worker_recv, false);
    worker_add_native(w, "tryRecv", (void *)nm_worker_try_recv, false);
    worker_add_native(w, "on", (void *)nm_worker_on, false);
    worker_add_native(w, "stop", (void *)nm_worker_stop, false);
    worker_add_native(w, "isrunning", (void *)nm_worker_is_running, false);
    worker_add_native(w, "id", (void *)nm_worker_id, false);
    worker_add_native(w, "self", (void *)nm_worker_self, true);
    tphp_class_register(w);
    g_worker_cls = w;

    TClass *c = worker_make_class("Tphp\\Worker\\Context");
    worker_add_native(c, "post", (void *)nm_worker_post, false);
    worker_add_native(c, "recv", (void *)nm_worker_recv, false);
    worker_add_native(c, "isrunning", (void *)nm_worker_is_running, false);
    worker_add_native(c, "id", (void *)nm_worker_id, false);
    tphp_class_register(c);
    g_wctx_cls = c;

    /* Tphp\Thread：真线程池（真并行，与协程组 TaskGroup 互补） */
    TClass *th = worker_make_class("Tphp\\Thread");
    th->m_ctor = (int16_t)th->n_methods;
    worker_add_native(th, "__construct", (void *)nm_thread_construct, false);
    worker_add_native(th, "spawn", (void *)nm_thread_spawn, false);
    worker_add_native(th, "wait", (void *)nm_thread_wait, false);
    worker_add_native(th, "detach", (void *)nm_thread_detach, false);
    worker_add_native(th, "count", (void *)nm_thread_count, false);
    worker_add_native(th, "poll", (void *)nm_thread_poll, false);
    tphp_class_register(th);
    g_thread_cls = th;
}
