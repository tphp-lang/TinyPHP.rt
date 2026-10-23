/*
 * TinyPHP.rt - sched.c - M4 协程调度器（fiber + libuv 事件循环）
 *
 * 模型（txiki.js 式三角色）：
 *   - 宿主线程：tphp_vm_run 准备好 main frame 后 tphp_sched_run() 首跳
 *     进调度器 fiber，全部协程完成后跳回，取退出码。
 *   - 调度器 fiber（单栈循环）：弹就绪协程 → jump 过去跑；就绪空则
 *     uv_run(UV_RUN_ONCE) 等事件（timer/IO 回调把等待者放回就绪队列）；
 *     无事件且仍有等待者 → 死锁 fatal（M4-c 起改抛 DeadlockError）。
 *   - 协程 fiber：跑 VM 帧。挂起点都在 tphp_fiber_suspend()（builtin
 *     深处冻结 C 栈，唤醒后从冻结点继续）。每个协程一份 TExecCtx，
 *     调度器 resume/收回时与 vm->x 整体交换（O(1) 扁平拷贝）。
 *
 * 所有权约定：
 *   - spawn 的 callable/实参从 builtin 参数区 ADDREF（builtin 参数由
 *     DO_FCALL 调用方释放），协程根帧构建时转移所有权。
 *   - 协程结果：根帧 RETURN 经 x.coro_result 移入 cont->result；
 *     await/getResult 返回副本。
 *   - 未捕获异常：TExObj 指针从 x.exception 移入 cont->exception；
 *     await 重抛时 ADDREF 副本（cont 仍持有原引用）。
 */
#include "tphp.h"
#include "value.h"
#include "opcode.h"
#include "class.h"
#include "fiber.h"
#include "tsched.h"
#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vm.c */
VM *tphp_vm_instance(void);
void tphp_vm_run_current(void);     /* main 协程体：跑已准备的 main frame */
void tphp_vm_unwind_current(void);  /* 外圈 setjmp 落地：就地展开 */
TStr *tstr_new(const char *s, size_t len);

/* ---- 协程状态 ---- */
typedef enum { TCS_READY, TCS_RUNNING, TCS_WAITING, TCS_DONE } TContState;

typedef struct TCont {
    TFiber *fib;
    TExecCtx x;            /* 挂起时的执行状态（与 vm->x 交换） */
    TVal result;           /* 协程返回值（x.coro_result 指向这里） */
    TExObj *exception;     /* 完成时若有未捕获异常则移到这里 */
    TObj *coro_obj;        /* Tphp\Coroutine 包装对象（自持 +1 引用） */
    int id;                /* 句柄 id（coro_obj->props[0]，注册表下标） */
    TContState state;
    struct TCont *qnext;   /* 就绪队列链 */
    struct TCont *waiter;  /* await 本协程的协程（M4-c：any 可作多协程共同等待者） */
    bool ex_consumed;      /* 异常已被 await/getResult 重抛（消费） */
    /* M4-c: 协作式取消 */
    bool cancel_pending;   /* 下一个挂起恢复点抛 AsyncCancelled */
    TStr *cancel_reason;   /* 取消原因（+1 引用，抛出后释放） */
    SchedEvent *waiting_on;/* 挂在哪个事件上（cancel 摘除用；NULL=非事件挂起） */
    struct TCont *await_target; /* await 挂起时等待的目标（cancel 摘除用） */
    bool multi_wait;       /* any(): 挂在多个协程的 waiter 槽上（cancel 可唤醒） */
    /* Channel 等待队列链 + 交接值：recv 挂起时是收到的值，send 挂起时
     * 是待发值（+1 引用）。一个协程同时只挂在一个 Channel 上，共用一槽。 */
    struct TCont *chan_next;
    TVal chan_val;
    /* M4-c: 死锁报告用挂起位置 */
    const char *sus_fn;
    int sus_line;
    /* 启动参数（spawn 时 ADDREF，根帧构建时转移） */
    TVal callable;
    TVal start_args[8];
    int n_start_args;
} TCont;

/* M5: every scheduler global is per-OS-thread (_Thread_local) — a worker
 * thread runs its own loop/fiber/ready-queue instance; fiber switches stay
 * inside one OS thread so TLS is stable across switches */
static _Thread_local uv_loop_t g_loop;
static _Thread_local bool g_loop_inited;            /* loop 是否有效（cleanup 后复位） */
static _Thread_local TFiber *g_sched;          /* 调度器 fiber */
static _Thread_local TCont *g_ready_head, *g_ready_tail;
static _Thread_local TCont *g_cur;             /* 正在运行的协程（协程视角=self） */
static _Thread_local TCont **g_conts;          /* id → cont 注册表 */
static _Thread_local int g_n_conts, g_cap_conts;
static _Thread_local int g_waiting;            /* WAITING 状态协程数（死锁判据） */
static _Thread_local bool g_any_uncaught;
static _Thread_local bool g_sched_active;      /* 事件循环可用（sleep 走 uv_timer） */
static _Thread_local TClass *g_coro_cls;       /* Tphp\Coroutine 内部类 */

/* ---- 就绪队列 ---- */

static void sched_ready(TCont *c) {
    if (c->state == TCS_DONE) return;
    c->state = TCS_READY;
    c->qnext = NULL;
    if (g_ready_tail) g_ready_tail->qnext = c;
    else g_ready_head = c;
    g_ready_tail = c;
}

/* ---- 协程完成 ---- */

static void cont_finish(TCont *c) {
    c->state = TCS_DONE;
    bool un = c->x.uncaught;
    /* main 协程的未捕获异常已在 unwind 就地 fatal，不保留（否则
     * sched_report_uncaught 会重复报告）；子协程保留给 await/report */
    c->exception = (un && c->x.coro_result) ? c->x.exception : NULL;
    if (c->x.calls) tm_free(c->x.calls);
    if (c->x.args) tm_free(c->x.args);
    if (c->x.caps) tm_free(c->x.caps);
    memset(&c->x, 0, sizeof c->x);
    if (c->waiter) {
        TCont *w = c->waiter;
        c->waiter = NULL;
        g_waiting--;
        sched_ready(w);
    }
}

/* ---- 调度器 ↔ 协程切换 ----
 * 无 tmp 交换：载入 c 的挂起状态；switch 返回（挂起/完成）后把最新
 * 状态收回 c->x，再把 vm->x 清成空闲态（下一次 resume 整体覆盖）。 */

static void resume_cont(TCont *c) {
    VM *vm = tphp_vm_instance();
    g_cur = c;
    c->state = TCS_RUNNING;
    vm->x = c->x;
    tphp_fiber_switch(c->fib, c);
    c->x = vm->x;
    memset(&vm->x, 0, sizeof vm->x);
    vm->x.done = true;
    if (c->x.done) cont_finish(c);
    g_cur = NULL;
}

/* 通用挂起点：状态保持 live 在 vm->x，由 resume_cont 收回 */
static void sched_suspend(TContState st) {
    TCont *c = g_cur;
    c->state = st;
    tphp_fiber_suspend(c);
}

/* ---- 协程体 ---- */

/* 构建 spawn 协程的根帧（callable 解析失败 → 异常落地 uncaught） */
static void build_root_frame(VM *vm, TCont *c) {
    TFunc *fn = NULL;
    TClosure *clo = NULL;
    if (c->callable.t == T_CLOSURE) {
        clo = (TClosure *)c->callable.u.p;
        if (!clo->fn || clo->bfn)
            tphp_vm_raise(vm, "Error",
                          "Tphp\\spawn expects a user function or closure");
        fn = clo->fn;
    } else {
        TStr *nm = (TStr *)c->callable.u.p;
        for (int i = 0; i < vm->prog->n_funcs; i++) {
            TFunc *f = vm->prog->funcs[i];
            if (f->name && !f->is_closure && tstr_eq(f->name, nm)) {
                fn = f;
                break;
            }
        }
        if (!fn)
            tphp_vm_raise(vm, "Error", "Call to undefined function %s()",
                          tstr_val(nm));
    }
    VMFrame *f = tm_malloc(sizeof *f);
    f->fn = fn;
    f->file = fn->file;           /* 根帧源文件：异常归属/raise_file 依赖，
                                   * tm_malloc 不初始化，漏设会读到已释放填充值 */
    /* 可见性作用域：闭包 bindTo/bind 重绑优先，否则声明类（闭包体里访问
     * 声明类的 private 成员依赖它）；按名调用的普通函数无类作用域 */
    TClass *scope = NULL;
    if (clo) scope = clo->scope ? clo->scope : (TClass *)fn->decl_cls;
    f->vis_scope = scope;
    f->pc = 0;
    f->prev = NULL;               /* 协程根：底帧 RETURN → coro_result */
    f->args_base = 0;
    f->called_cls = scope;        /* static:: 后绑定与声明类一致 */
    f->n_calls_at_entry = 0;
    uint32_t nslots = (uint32_t)fn->n_locals + fn->n_tmps;
    f->slots = tm_malloc(sizeof(TVal) * nslots);
    for (uint32_t i = 0; i < nslots; i++)
        f->slots[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    if (clo)
        for (uint16_t i = 0; i < clo->n_cap; i++) {
            tval_addref(clo->captures[i]);
            f->slots[i] = clo->captures[i];
        }
    /* 实参转移进 args 区（spawn 时已 ADDREF，此处 move） */
    for (int i = 0; i < c->n_start_args; i++) {
        vm->x.args[i] = c->start_args[i];
        c->start_args[i] = (TVal){ .u.i = 0, .t = T_UNDEF };
    }
    vm->x.n_args = c->n_start_args;
    f->n_args_sent = (uint16_t)c->n_start_args;
    vm->x.frame = f;
}

static void cont_entry(TFiber *fb) {
    TCont *c = (TCont *)tphp_fiber_data(fb);
    VM *vm = tphp_vm_instance();
    tphp_ctx_ensure(&vm->x);          /* 本协程自己的堆数组 */
    vm->x.coro_result = &c->result;   /* 根帧 RETURN 写这里 */
    if (setjmp(vm->x.unwind) == 0) {
        build_root_frame(vm, c);
        tval_release(&c->callable);   /* 已捕获进帧/已解析 */
        c->callable = (TVal){ .u.i = 0, .t = T_UNDEF };
        tphp_vm_run_current();
    } else {
        /* build_root_frame 的 raise 落在外圈：就地展开到底（done/uncaught
         * 由 vm_unwind 置位，异常保留给 await） */
        tphp_vm_unwind_current();
    }
    /* 释放 args 区残留（根帧未消费的实参）；帧/调用栈由 RETURN/unwind
     * 清理，数组由 cont_finish 归还 */
    for (int i = 0; i < vm->x.n_args; i++)
        if (vm->x.args[i].t != T_UNDEF) tval_release(&vm->x.args[i]);
    vm->x.n_args = 0;
}

static void main_entry(TFiber *fb) {
    (void)tphp_fiber_data(fb);
    tphp_vm_run_current();            /* setjmp + 跑 main frame */
}

/* ---- 调度器循环 ---- */

static void sched_entry(TFiber *fb) {
    (void)fb;
    for (;;) {
        if (g_ready_head) {
            TCont *c = g_ready_head;
            g_ready_head = c->qnext;
            if (!g_ready_head) g_ready_tail = NULL;
            resume_cont(c);
            continue;
        }
        if (g_waiting == 0) break;    /* 全部完成 */
        /* 等事件：UV_RUN_ONCE 阻塞到下一个 timer/IO 事件。
         * 注意：最后一个 fs 请求完成的那轮 uv_run 返回 0（loop 里已无
         * 句柄），但 fire 已经把等待者放进就绪队列——先看队列再判定 */
        int r = uv_run(&g_loop, UV_RUN_ONCE);
        if (r == 0) {
            if (g_ready_head) continue;         /* 本轮 fire 的等待者待跑 */
            if (g_waiting == 0) break;          /* 全部完成 */
            fflush(stdout);
            fprintf(stderr,
                    "PHP Fatal error:  Uncaught DeadlockError: %d "
                    "coroutine(s) suspended with no pending events\n",
                    g_waiting);
            /* 标注各协程的挂起位置 */
            for (int i = 0; i < g_n_conts; i++) {
                TCont *c = g_conts[i];
                if (c->state == TCS_DONE) continue;
                fprintf(stderr, "  coroutine #%d suspended in %s:%d\n",
                        c->id, c->sus_fn ? c->sus_fn : "?", c->sus_line);
            }
            exit(255);
        }
    }
    /* 全部完成：跳回宿主（fiber_entry 的 back = 宿主首跳快照） */
}

/* ---- 通用事件挂起（SchedEvent）----
 * sleep 的 timer、io.c 的 uv_fs 回调都走这条通道：等待者挂起（g_waiting++），
 * loop 回调里 fire（g_waiting-- + 就绪队列）。事件结构可放在等待协程的
 * 栈上——挂起期间栈冻结，回调跨 fiber 写 fired/ud 是安全的。 */

/* 挂起前记录协程的 PHP 挂起位置（死锁报告用） */
static void record_suspend_pos(TCont *c) {
    VM *vm = tphp_vm_instance();
    if (vm->x.frame && vm->x.frame->fn && vm->x.frame->pc < vm->x.frame->fn->n_ops) {
        c->sus_fn = vm->x.frame->fn->name ? tstr_val(vm->x.frame->fn->name)
                                          : "{closure}";
        c->sus_line = vm->x.frame->fn->ops[vm->x.frame->pc].line;
    } else {
        c->sus_fn = "?";
        c->sus_line = 0;
    }
}

/* 挂起恢复后在协程自己的栈上调用：若被 cancel → 弹出本挂起 native 遗留
 * 的调用 rec（挂起跳出时它在栈顶；正常恢复不弹——native 还要继续）并抛
 * AsyncCancelled（longjmp 到本协程的 unwind 落点）。 */
static void cont_check_cancel(VM *vm, TCont *c) {
    if (!c->cancel_pending) return;
    c->cancel_pending = false;
    TStr *reason = c->cancel_reason;
    c->cancel_reason = NULL;
    if (vm->x.n_calls > 0) {
        VMCallRec *rec = &vm->x.calls[vm->x.n_calls - 1];
        vm->x.n_args = (int)rec->args_base;   /* 跳过的实参值引用泄漏同
                                                 raise-from-builtin 策略 */
        vm->x.n_calls--;
    }
    if (reason)
        tphp_vm_raise(vm, "AsyncCancelled", "%s", tstr_val(reason));
    tphp_vm_raise(vm, "AsyncCancelled", "coroutine was cancelled");
}

void tphp_sched_event_reset(SchedEvent *ev) {
    ev->fired = false;
    ev->waiter = NULL;
}

void tphp_sched_event_wait(SchedEvent *ev) {
    if (ev->fired) return;
    TCont *c = g_cur;
    record_suspend_pos(c);
    c->waiting_on = ev;
    ev->waiter = c;
    g_waiting++;
    sched_suspend(TCS_WAITING);
    c->waiting_on = NULL;
    cont_check_cancel(tphp_vm_instance(), c);
}

void tphp_sched_event_fire(SchedEvent *ev) {
    if (ev->fired) return;
    ev->fired = true;
    if (ev->waiter) {
        TCont *w = (TCont *)ev->waiter;
        ev->waiter = NULL;
        g_waiting--;
        sched_ready(w);
    }
}

/* ---- 细粒度挂起原语（net.c 等） ---- */

TCont *tphp_sched_current(void) { return g_cur; }

void tphp_sched_ready(TCont *c) { sched_ready(c); }

void tphp_sched_wake(TCont *c) {
    g_waiting--;
    sched_ready(c);
}

void tphp_sched_wait_begin(void) {
    record_suspend_pos(g_cur);
    g_waiting++;
}

void tphp_sched_wait_suspend(void) {
    sched_suspend(TCS_WAITING);
}

void tphp_sched_check_cancel(void) {
    cont_check_cancel(tphp_vm_instance(), g_cur);
}

struct uv_loop_s *tphp_sched_loop(void) { return &g_loop; }
bool tphp_sched_active(void) { return g_sched_active; }

/* ---- 定时器挂起（sleep/usleep 的底层） ---- */

static void timer_close_cb(uv_handle_t *h) { tm_free(h); }

static void timer_cb(uv_timer_t *t) {
    SchedEvent *ev = (SchedEvent *)t->data;
    uv_close((uv_handle_t *)t, timer_close_cb);
    tphp_sched_event_fire(ev);
}

/* 挂起当前协程 ms 毫秒。无调度器时退化为阻塞 Sleep（不应发生）。 */
static void suspend_for_ms(int64_t ms) {
    if (ms < 0) ms = 0;
    if (!g_sched_active) {
#ifdef _WIN32
        Sleep((DWORD)ms);
#else
        struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
#endif
        return;
    }
    SchedEvent ev;
    tphp_sched_event_reset(&ev);
    uv_timer_t *t = tm_malloc(sizeof *t);
    memset(t, 0, sizeof *t);
    uv_timer_init(&g_loop, t);
    t->data = &ev;
    uv_timer_start(t, timer_cb, (uint64_t)ms, 0);
    tphp_sched_event_wait(&ev);
}

/* ---- Tphp\Coroutine 内部类 ----
 * __handle 属性存协程 id（int）；四个方法全部 native。 */

static TCont *cont_of(TObj *o) {
    if (o->props[0].t != T_INT)
        tphp_vm_raise(tphp_vm_instance(), "Error",
                      "Tphp\\Coroutine object was not created by Tphp\\spawn");
    return g_conts[o->props[0].u.i];
}

static TVal nm_coro_isdone(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return cont_of((TObj *)self)->state == TCS_DONE ? TV_TRUE : TV_FALSE;
}

static TVal nm_coro_isrunning(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    return cont_of((TObj *)self)->state != TCS_DONE ? TV_TRUE : TV_FALSE;
}

static TVal nm_coro_getresult(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TCont *c = cont_of((TObj *)self);
    if (c->state != TCS_DONE)
        tphp_vm_raise(vm, "Error", "Coroutine is not done yet");
    if (c->exception) {
        /* 与 await 一致：结果未产出，异常透传 */
        c->ex_consumed = true;
        TVal exv = TV_OBJ(c->exception);
        tval_addref(exv);
        vm->x.exception = (TExObj *)exv.u.p;
        longjmp(vm->x.unwind, 1);
    }
    tval_addref(c->result);
    return c->result;
}

static TVal nm_coro_getexception(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TCont *c = cont_of((TObj *)self);
    if (c->state != TCS_DONE || !c->exception) return TV_NULL;
    TVal exv = TV_OBJ(c->exception);
    tval_addref(exv);
    return exv;
}

static void add_native_method(TClass *cls, const char *name, void *native) {
    TMethod *ms = tm_malloc(sizeof(TMethod) * (size_t)(cls->n_methods + 1));
    if (cls->methods) {
        memcpy(ms, cls->methods, sizeof(TMethod) * (size_t)cls->n_methods);
        tm_free(cls->methods);
    }
    TMethod *m = &ms[cls->n_methods++];
    memset(m, 0, sizeof *m);
    m->name = tstr_new(name, strlen(name));
    m->visibility = 0;            /* CVIS_PUBLIC */
    m->native = native;
    cls->methods = ms;
}

static void register_chan_class(void);
static void register_tg_class(void);
static void register_task_class(void);
static _Thread_local TClass *g_chan_cls;  /* Tphp\Channel 内部类（await 分派用） */
static _Thread_local TClass *g_tg_cls;    /* Tphp\TaskGroup 内部类 */
static _Thread_local TClass *g_task_cls;  /* Tphp\Task 内部类（async() 包装） */
/* forward: native 方法（定义在后面的段里） */
static TVal nm_coro_cancel(VM *vm, void *self, TVal *args, int n);
static TVal nm_coro_wait(VM *vm, void *self, TVal *args, int n);
static TVal nm_chan_construct(VM *vm, void *self, TVal *args, int n);
static TVal nm_chan_send(VM *vm, void *self, TVal *args, int n);
static TVal nm_chan_recv(VM *vm, void *self, TVal *args, int n);
static TVal nm_chan_close(VM *vm, void *self, TVal *args, int n);
static TVal nm_chan_len(VM *vm, void *self, TVal *args, int n);
static TVal nm_tg_construct(VM *vm, void *self, TVal *args, int n);
static TVal nm_tg_spawn(VM *vm, void *self, TVal *args, int n);
static TVal nm_tg_wait(VM *vm, void *self, TVal *args, int n);
static TVal nm_tg_count(VM *vm, void *self, TVal *args, int n);
static TVal nm_task_invoke(VM *vm, void *self, TVal *args, int n);

static void register_coro_class(void) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern("Tphp\\Coroutine", 14);
    cls->kind = 0;                /* CK_CLASS */
    cls->flattened = true;
    cls->m_ctor = -1; cls->m_dtor = -1; cls->m_get = -1; cls->m_set = -1;
    cls->m_isset = -1; cls->m_unset = -1; cls->m_call = -1;
    cls->m_callstatic = -1; cls->m_tostring = -1; cls->m_invoke = -1;
    cls->m_clone = -1; cls->m_debuginfo = -1; cls->m_clonestate = -1;
    /* __handle 协程 id（int） */
    TProp *p = tm_malloc(sizeof(TProp));
    memset(p, 0, sizeof *p);
    p->name = tstr_new("__handle", 8);
    p->defval = TV_NULL;
    p->slot = 0;
    cls->props = p;
    cls->n_props = 1;
    add_native_method(cls, "isdone", (void *)nm_coro_isdone);
    add_native_method(cls, "isrunning", (void *)nm_coro_isrunning);
    add_native_method(cls, "getresult", (void *)nm_coro_getresult);
    add_native_method(cls, "getexception", (void *)nm_coro_getexception);
    add_native_method(cls, "cancel", (void *)nm_coro_cancel);
    add_native_method(cls, "wait", (void *)nm_coro_wait);   /* await 的对象式别名 */
    tphp_class_register(cls);
    g_coro_cls = cls;
}

static void register_chan_class(void) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern("Tphp\\Channel", 12);
    cls->kind = 0;                /* CK_CLASS */
    cls->flattened = true;
    cls->m_ctor = -1; cls->m_dtor = -1; cls->m_get = -1; cls->m_set = -1;
    cls->m_isset = -1; cls->m_unset = -1; cls->m_call = -1;
    cls->m_callstatic = -1; cls->m_tostring = -1; cls->m_invoke = -1;
    cls->m_clone = -1; cls->m_debuginfo = -1; cls->m_clonestate = -1;
    /* __handle：chan 注册表下标（native ctor 写入） */
    TProp *p = tm_malloc(sizeof(TProp));
    memset(p, 0, sizeof *p);
    p->name = tstr_new("__handle", 8);
    p->defval = TV_NULL;
    p->slot = 0;
    cls->props = p;
    cls->n_props = 1;
    cls->m_ctor = (int16_t)cls->n_methods;   /* ctor 先注册 */
    add_native_method(cls, "__construct", (void *)nm_chan_construct);
    add_native_method(cls, "send", (void *)nm_chan_send);
    add_native_method(cls, "recv", (void *)nm_chan_recv);
    add_native_method(cls, "close", (void *)nm_chan_close);
    add_native_method(cls, "len", (void *)nm_chan_len);
    tphp_class_register(cls);
    g_chan_cls = cls;
}

/* Tphp\TaskGroup —— 协程任务组（wait 收口）。`Thread` 是全局别名：
 * 名字叫线程，语义是单线程内并发（真 OS 线程用 tphp\Worker）。 */
static TClass *make_tg_class(const char *name, size_t len) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern(name, len);
    cls->kind = 0;                /* CK_CLASS */
    cls->flattened = true;
    cls->m_ctor = -1; cls->m_dtor = -1; cls->m_get = -1; cls->m_set = -1;
    cls->m_isset = -1; cls->m_unset = -1; cls->m_call = -1;
    cls->m_callstatic = -1; cls->m_tostring = -1; cls->m_invoke = -1;
    cls->m_clone = -1; cls->m_debuginfo = -1; cls->m_clonestate = -1;
    /* __tasks：组内协程对象数组（native ctor 建，spawn 追加） */
    TProp *p = tm_malloc(sizeof(TProp));
    memset(p, 0, sizeof *p);
    p->name = tstr_new("__tasks", 7);
    p->defval = TV_NULL;
    p->slot = 0;
    cls->props = p;
    cls->n_props = 1;
    cls->m_ctor = (int16_t)cls->n_methods;   /* ctor 先注册 */
    add_native_method(cls, "__construct", (void *)nm_tg_construct);
    add_native_method(cls, "spawn", (void *)nm_tg_spawn);
    add_native_method(cls, "wait", (void *)nm_tg_wait);
    add_native_method(cls, "count", (void *)nm_tg_count);
    tphp_class_register(cls);
    return cls;
}

static void register_tg_class(void) {
    g_tg_cls = make_tg_class("Tphp\\TaskGroup", 14);
    make_tg_class("Tphp\\Thread", 11);
}

/* Tphp\Task —— async($callable) 的返回物：调用即启动协程。
 *   $p1 = async(p1(...));
 *   $p1(3, 4);              // 等价 spawn(p1(...), 3, 4)
 *   $p1(3, 4)->wait();      // 启动并等待
 * 这是"函数调用即并发"唯一不破坏求值顺序的写法：包装一次，调用任意次。 */
static void register_task_class(void) {
    TClass *cls = tm_malloc(sizeof *cls);
    memset(cls, 0, sizeof *cls);
    cls->name = tstr_intern("Tphp\\Task", 9);
    cls->kind = 0;
    cls->flattened = true;
    cls->m_ctor = -1; cls->m_dtor = -1; cls->m_get = -1; cls->m_set = -1;
    cls->m_isset = -1; cls->m_unset = -1; cls->m_call = -1;
    cls->m_callstatic = -1; cls->m_tostring = -1; cls->m_invoke = -1;
    cls->m_clone = -1; cls->m_debuginfo = -1; cls->m_clonestate = -1;
    /* __fn：被包装的 callable */
    TProp *p = tm_malloc(sizeof(TProp));
    memset(p, 0, sizeof *p);
    p->name = tstr_new("__fn", 4);
    p->defval = TV_NULL;
    p->slot = 0;
    cls->props = p;
    cls->n_props = 1;
    add_native_method(cls, "__invoke", (void *)nm_task_invoke);
    cls->m_invoke = (int16_t)(cls->n_methods - 1);
    tphp_class_register(cls);
    g_task_cls = cls;
}

/* 内部类注册：必须在 tphp_compile 之前完成（A_NEW 有编译期类存在性
 * 检查）；幂等，tphp_sched_run 里再调一次也无妨 */
static _Thread_local bool g_async_inited;
void tphp_async_init(void) {
    if (g_async_inited) return;
    g_async_inited = true;
    register_coro_class();
    register_chan_class();
    register_tg_class();
    register_task_class();
}

/* ---- id 注册表 ---- */

static TCont *cont_new(void) {
    TCont *c = tm_malloc(sizeof *c);
    memset(c, 0, sizeof *c);
    c->state = TCS_READY;
    c->chan_val = (TVal){ .u.i = 0, .t = T_UNDEF };
    if (g_n_conts >= g_cap_conts) {
        g_cap_conts = g_cap_conts ? g_cap_conts * 2 : 16;
        g_conts = tm_realloc(g_conts, sizeof(TCont *) * (size_t)g_cap_conts);
    }
    c->id = g_n_conts;
    g_conts[g_n_conts++] = c;
    return c;
}

/* ---- Tphp\ 内置 ---- */

/* forward: Tphp\Channel recv 实现（Channel 段） */
static TVal chan_recv_impl(VM *vm, void *self);

TVal tphp_async_spawn(VM *vm, TVal *args, int n) {
    if (n < 1 || (args[0].t != T_CLOSURE && args[0].t != T_STR))
        tphp_vm_raise(vm, "TypeError",
                      "Tphp\\spawn expects a callable as its first argument");
    if (n - 1 > 8)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Tphp\\spawn supports at most 8 extra arguments");
    TCont *c = cont_new();
    c->callable = args[0];
    tval_addref(c->callable);
    c->n_start_args = n - 1;
    for (int i = 0; i < c->n_start_args; i++) {
        c->start_args[i] = args[i + 1];
        tval_addref(c->start_args[i]);
    }
    /* Coroutine 包装对象：__handle = id */
    TObj *o = tphp_obj_alloc(g_coro_cls);
    o->props[0] = TV_INT(c->id);
    o->h.rc++;                    /* TCont 自持一份 */
    c->coro_obj = o;
    c->fib = tphp_fiber_new(tphp_fiber_default_stack, cont_entry, c);
    sched_ready(c);
    return TV_OBJ(o);             /* PHP 侧拿走 alloc 的那一份 */
}

/* await 一个协程：挂起等待 → 异常透传 / 返回结果。
 * 挂起时在 target->waiter 登记（cancel 摘除用），恢复后检查取消。 */
static TVal await_one_coro(VM *vm, TCont *target) {
    if (target->state != TCS_DONE) {
        if (target->waiter && target->waiter != g_cur)
            tphp_vm_raise(vm, "Error", "Coroutine is already being awaited");
        g_waiting++;
        target->waiter = g_cur;
        g_cur->await_target = target;
        record_suspend_pos(g_cur);
        sched_suspend(TCS_WAITING);   /* 唤醒即 target 已完成（或被取消） */
        g_cur->await_target = NULL;
        cont_check_cancel(vm, g_cur);
    }
    if (target->exception) {
        /* 异常透传：TExObj 从挂起协程转移到当前协程的 unwind */
        target->ex_consumed = true;
        TVal exv = TV_OBJ(target->exception);
        tval_addref(exv);
        vm->x.exception = (TExObj *)exv.u.p;
        longjmp(vm->x.unwind, 1);
    }
    tval_addref(target->result);
    return target->result;
}

/* await(Coroutine|Channel) */
TVal tphp_async_await(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_OBJ)
        tphp_vm_raise(vm, "TypeError",
                      "Tphp\\await expects an Tphp\\Coroutine or Channel");
    TObj *o = (TObj *)args[0].u.p;
    if (o->cls == g_coro_cls)
        return await_one_coro(vm, cont_of(o));
    if (o->cls == g_chan_cls)
        return chan_recv_impl(vm, o);
    tphp_vm_raise(vm, "TypeError",
                  "Tphp\\await expects an Tphp\\Coroutine or Channel");
}

TVal tphp_async_current(VM *vm, TVal *args, int n) {
    (void)args; (void)n;
    if (!g_cur)
        tphp_vm_raise(vm, "Error", "no current coroutine");
    if (!g_cur->coro_obj) {
        /* 惰性创建（main 协程；spawn 出的已在 spawn 时创建） */
        TObj *o = tphp_obj_alloc(g_coro_cls);
        o->props[0] = TV_INT(g_cur->id);
        o->h.rc++;
        g_cur->coro_obj = o;
    }
    TVal r = TV_OBJ(g_cur->coro_obj);
    tval_addref(r);
    return r;
}

/* all(array $coros): array —— 全部完成后按输入序返回值数组。
 * 协程是并发跑的：依次 await 只是顺序收割（总耗时 = 最慢者）。 */
TVal tphp_async_all(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_ARR)
        tphp_vm_raise(vm, "TypeError", "Tphp\\all expects an array");
    TArr *in = (TArr *)args[0].u.p;
    TArr *out = tarr_new();
    for (uint32_t i = 0; i < in->dused; i++) {
        TVal v = in->data[i];
        if (v.t == T_UNDEF) { tarr_append(out, TV_NULL); continue; }
        if (v.t != T_OBJ || ((TObj *)v.u.p)->cls != g_coro_cls)
            tphp_vm_raise(vm, "TypeError",
                          "Tphp\\all expects an array of Tphp\\Coroutine");
        tarr_append(out, await_one_coro(vm, cont_of((TObj *)v.u.p)));
    }
    return TV_ARR(out);
}

/* any(array $coros): mixed —— 首个成功者即返回其值；全部失败则抛
 * 最后一个异常。watcher 登记为每个未完成协程的 waiter（multi_wait）。 */
TVal tphp_async_any(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_ARR)
        tphp_vm_raise(vm, "TypeError", "Tphp\\any expects an array");
    TArr *in = (TArr *)args[0].u.p;
    int cnt = 0;
    TCont **list = tm_malloc(sizeof(TCont *) * (size_t)(in->dused + 1));
    for (uint32_t i = 0; i < in->dused; i++) {
        TVal v = in->data[i];
        if (v.t == T_UNDEF) continue;
        if (v.t != T_OBJ || ((TObj *)v.u.p)->cls != g_coro_cls) {
            tm_free(list);
            tphp_vm_raise(vm, "TypeError",
                          "Tphp\\any expects an array of Tphp\\Coroutine");
        }
        list[cnt++] = cont_of((TObj *)v.u.p);
    }
    if (cnt == 0) {
        tm_free(list);
        tphp_vm_raise(vm, "Error", "Tphp\\any expects a non-empty array");
    }
    TVal out = TV_NULL;
    TExObj *last_ex = NULL;
    TCont *last_ex_owner = NULL;
    for (;;) {
        int pending = 0;
        for (int i = 0; i < cnt; i++) {
            TCont *c = list[i];
            if (c->state == TCS_DONE) {
                if (!c->exception) {
                    out = c->result;          /* 首个成功 */
                    tval_addref(out);
                    goto done;
                }
                last_ex = c->exception;
                last_ex_owner = c;
            } else {
                if (c->waiter && c->waiter != g_cur) {
                    tm_free(list);
                    tphp_vm_raise(vm, "Error",
                                  "Coroutine is already being awaited");
                }
                c->waiter = g_cur;            /* 登记（保持） */
                pending++;
            }
        }
        if (pending == 0) {
            /* 全部失败：抛最后一个异常；其余异常同样视为已消费 */
            if (last_ex) {
                for (int i = 0; i < cnt; i++) {
                    if (list[i]->waiter == g_cur) list[i]->waiter = NULL;
                    if (list[i]->exception) list[i]->ex_consumed = true;
                }
                last_ex_owner->ex_consumed = true;
                TVal exv = TV_OBJ(last_ex);
                tval_addref(exv);
                tm_free(list);
                vm->x.exception = last_ex;
                longjmp(vm->x.unwind, 1);
            }
            goto done;                        /* 空转保护（不会到达） */
        }
        /* 挂起：任一协程完成都会唤醒（cont_finish 唤醒 waiter） */
        g_cur->multi_wait = true;
        record_suspend_pos(g_cur);
        g_waiting++;
        sched_suspend(TCS_WAITING);
        g_cur->multi_wait = false;
        /* 恢复：先解除全部登记（cancel 抛出也安全），再查取消 */
        for (int i = 0; i < cnt; i++)
            if (list[i]->waiter == g_cur) list[i]->waiter = NULL;
        cont_check_cancel(vm, g_cur);
    }
done:
    for (int i = 0; i < cnt; i++)
        if (list[i]->waiter == g_cur) list[i]->waiter = NULL;
    tm_free(list);
    return out;
}

/* run(callable $main): 脚本顶层本就是 main 协程、事件循环常驻——
 * 等价于 spawn + await（丢弃返回值）。 */
TVal tphp_async_run(VM *vm, TVal *args, int n) {
    TVal h = tphp_async_spawn(vm, args, n);
    TVal r = tphp_async_await(vm, &h, 1);
    tval_release(&r);
    return TV_NULL;
}

TVal tphp_async_suspend(VM *vm, TVal *args, int n) {
    (void)args; (void)n;
    sched_ready(g_cur);               /* 立即重新入队（协作式让出一次） */
    record_suspend_pos(g_cur);
    sched_suspend(TCS_READY);
    cont_check_cancel(vm, g_cur);
    return TV_NULL;
}

TVal tphp_async_sleep(VM *vm, TVal *args, int n) {
    int64_t sec = n > 0 ? tval_to_int(args[0]) : 0;
    (void)vm;
    suspend_for_ms(sec * 1000);
    return TV_NULL;
}

TVal tphp_async_usleep(VM *vm, TVal *args, int n) {
    int64_t usec = n > 0 ? tval_to_int(args[0]) : 0;
    (void)vm;
    suspend_for_ms(usec / 1000);
    return TV_NULL;
}

/* ---- Tphp\Channel（M4-c）----
 * 环形缓冲；cap=0 无界。单等待者模型：满挂 send、空挂 recv。
 * closed 后 send 抛错、recv 耗尽返回 null。 */

typedef struct TChan {
    TVal *buf;             /* 动态环形槽（无界时随 len 扩容） */
    int tcap;              /* buf 已分配槽位（2 的幂；0 = 未分配） */
    int cap;               /* 0 = 无界；>0 = 有界上限 */
    int len, head;
    bool closed;
    /* 等待者 FIFO：send 满时入队；recv 空时入队。多消费者/多生产者都支持，
     * send 直接交接给队首 recv（值走 TCont.chan_val），否则入缓冲。 */
    TCont *send_head, *send_tail;
    TCont *recv_head, *recv_tail;
} TChan;

static _Thread_local TChan **g_chans;
static _Thread_local int g_n_chans, g_cap_chans;

static int chan_new(int cap) {
    if (g_n_chans >= g_cap_chans) {
        g_cap_chans = g_cap_chans ? g_cap_chans * 2 : 8;
        g_chans = tm_realloc(g_chans, sizeof(TChan *) * (size_t)g_cap_chans);
    }
    TChan *ch = tm_malloc(sizeof *ch);
    memset(ch, 0, sizeof *ch);
    ch->cap = cap;
    g_chans[g_n_chans] = ch;
    return g_n_chans++;
}

static TChan *chan_of(VM *vm, void *self) {
    TObj *o = (TObj *)self;
    if (o->props[0].t != T_INT)
        tphp_vm_raise(vm, "Error", "Channel object was not constructed");
    return g_chans[o->props[0].u.i];
}

/* 环形队列写：必要时扩容（无界 channel 随 len 增长） */
static void chan_push(TChan *ch, TVal v) {
    if (ch->len == ch->tcap) {
        int ncap = ch->tcap ? ch->tcap * 2 : 8;
        TVal *nb = tm_malloc(sizeof(TVal) * (size_t)ncap);
        for (int k = 0; k < ch->len; k++)
            nb[k] = ch->buf[(ch->head + k) % ch->tcap];
        tm_free(ch->buf);
        ch->buf = nb;
        ch->tcap = ncap;
        ch->head = 0;
    }
    ch->buf[(ch->head + ch->len) % ch->tcap] = v;   /* move，不 addref */
    ch->len++;
}

/* 环形队列读（len > 0） */
static TVal chan_pop(TChan *ch) {
    TVal v = ch->buf[ch->head];
    ch->buf[ch->head] = (TVal){ .u.i = 0, .t = T_UNDEF };
    ch->head = (ch->head + 1) % ch->tcap;
    ch->len--;
    return v;
}

/* 等待者 FIFO（多生产者/多消费者） */
static void chan_q_push(TCont **head, TCont **tail, TCont *c) {
    c->chan_next = NULL;
    if (*tail) (*tail)->chan_next = c;
    else *head = c;
    *tail = c;
}

static TCont *chan_q_pop(TCont **head, TCont **tail) {
    TCont *c = *head;
    if (!c) return NULL;
    *head = c->chan_next;
    if (!*head) *tail = NULL;
    c->chan_next = NULL;
    return c;
}

static TVal nm_chan_construct(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    int64_t cap = 0;
    if (n > 0) {
        if (args[0].t != T_INT)
            tphp_vm_raise(vm, "TypeError",
                          "Tphp\\Channel::__construct expects an int capacity");
        cap = args[0].u.i;
        if (cap < 0)
            tphp_vm_raise(vm, "ValueError",
                          "Tphp\\Channel capacity must be >= 0");
    }
    o->props[0] = TV_INT(chan_new((int)cap));
    return TV_NULL;
}

/* send(mixed $v): 满则挂起；closed 抛错 */
static TVal nm_chan_send(VM *vm, void *self, TVal *args, int n) {
    TChan *ch = chan_of(vm, self);
    if (n < 1)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "Tphp\\Channel::send expects a value");
    if (ch->closed)
        tphp_vm_raise(vm, "Error", "send to a closed channel");
    TCont *w = chan_q_pop(&ch->recv_head, &ch->recv_tail);
    if (w) {
        /* 有接收者在等：直接交接（值走它的 chan_val，args 由调用方释放） */
        w->chan_val = args[0];
        tval_addref(w->chan_val);
        g_waiting--;
        sched_ready(w);
        return TV_NULL;
    }
    if (ch->cap == 0 || ch->len < ch->cap) {
        chan_push(ch, args[0]);
        tval_addref(args[0]);
        return TV_NULL;
    }
    /* 满：入 send 队列挂起（多生产者 FIFO） */
    g_cur->chan_val = args[0];
    tval_addref(g_cur->chan_val);
    chan_q_push(&ch->send_head, &ch->send_tail, g_cur);
    record_suspend_pos(g_cur);
    g_waiting++;
    sched_suspend(TCS_WAITING);
    /* 恢复：值已被 recv 取走（chan_val 置 UNDEF）或 close 作废，此处收尾 */
    tval_release(&g_cur->chan_val);
    g_cur->chan_val = (TVal){ .u.i = 0, .t = T_UNDEF };
    if (ch->closed)
        tphp_vm_raise(vm, "Error", "send to a closed channel");
    cont_check_cancel(vm, g_cur);
    return TV_NULL;
}

/* recv 实现（await(Channel) 与 ->recv() 共用；TChan 从对象取） */
static TVal chan_recv_impl(VM *vm, void *self) {
    TChan *ch = chan_of(vm, self);
    TCont *s = chan_q_pop(&ch->send_head, &ch->send_tail);
    if (s) {
        /* 有发送者在等（有界满）：直接交接，缓冲不动 */
        TVal v = s->chan_val;
        s->chan_val = (TVal){ .u.i = 0, .t = T_UNDEF };
        g_waiting--;
        sched_ready(s);
        return v;
    }
    if (ch->len > 0)
        return chan_pop(ch);
    if (ch->closed)
        return TV_NULL;               /* closed 且空 → null */
    /* 空：入 recv 队列挂起（多消费者 FIFO） */
    g_cur->chan_val = TV_NULL;
    chan_q_push(&ch->recv_head, &ch->recv_tail, g_cur);
    record_suspend_pos(g_cur);
    g_waiting++;
    sched_suspend(TCS_WAITING);
    /* 恢复：send 交接值 / close 给 null */
    TVal v = g_cur->chan_val;
    g_cur->chan_val = (TVal){ .u.i = 0, .t = T_UNDEF };
    cont_check_cancel(vm, g_cur);
    return v;
}

static TVal nm_chan_recv(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    return chan_recv_impl(vm, self);
}

/* close(): closed 后 send 抛错、recv 耗尽返回 null；唤醒全部等待者 */
static TVal nm_chan_close(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TChan *ch = chan_of(vm, self);
    if (!ch->closed) {
        ch->closed = true;
        TCont *s;
        while ((s = chan_q_pop(&ch->send_head, &ch->send_tail)) != NULL) {
            /* 恢复后抛 closed；待发值由恢复点释放 */
            g_waiting--;
            sched_ready(s);
        }
        TCont *w;
        while ((w = chan_q_pop(&ch->recv_head, &ch->recv_tail)) != NULL) {
            w->chan_val = TV_NULL;    /* recv 恢复返回 null */
            g_waiting--;
            sched_ready(w);
        }
    }
    return TV_TRUE;
}

static TVal nm_chan_len(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TChan *ch = chan_of(vm, self);
    return TV_INT(ch->len);
}

/* ---- Coroutine::cancel（M4-c 协作式取消）----
 * 标记后：RUNNING(=self) 立即抛；WAITING 从挂起点摘除唤醒（恢复点抛）；
 * READY 下次运行抛。DONE 返回 false。 */
static TVal nm_coro_cancel(VM *vm, void *self, TVal *args, int n) {
    TCont *c = cont_of((TObj *)self);
    if (c->state == TCS_DONE) return TV_FALSE;
    c->cancel_pending = true;
    c->cancel_reason = NULL;
    if (n > 0 && args[0].t == T_STR) {
        c->cancel_reason = (TStr *)args[0].u.p;
        tval_addref(TV_STR(c->cancel_reason));
    }
    if (c == g_cur) {
        cont_check_cancel(vm, c);     /* 自取消：立即抛（noreturn） */
        return TV_TRUE;
    }
    if (c->state == TCS_WAITING) {
        if (c->waiting_on && !c->waiting_on->fired) {
            c->waiting_on->waiter = NULL;      /* 从事件摘除 */
            c->waiting_on = NULL;
            g_waiting--;
            sched_ready(c);
        } else if (c->await_target) {
            c->await_target->waiter = NULL;    /* 从 await 目标摘除 */
            c->await_target = NULL;
            g_waiting--;
            sched_ready(c);
        } else if (c->multi_wait) {
            c->multi_wait = false;             /* any(): 登记由恢复点清理 */
            g_waiting--;
            sched_ready(c);
        }
        /* 其他 WAITING（无唤醒通道）：挂起操作照常完成，恢复点抛 */
    }
    /* READY：下次运行时在挂起点抛 */
    return TV_TRUE;
}

/* Coroutine::wait() —— await 的对象式别名，返回协程结果（异常照常透传） */
static TVal nm_coro_wait(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    return await_one_coro(vm, cont_of((TObj *)self));
}

/* ---- Tphp\TaskGroup ---- */

static TVal nm_tg_construct(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    o->props[0] = TV_ARR(tarr_new());
    return TV_NULL;
}

/* spawn(callable $fn, ...$args): Coroutine —— 启动并登记进组，返回句柄。
 * 接受闭包/函数名，也接受 async() 包装出的 Tphp\Task（走其 __invoke）。 */
static TVal nm_tg_spawn(VM *vm, void *self, TVal *args, int n) {
    extern TVal tphp_vm_call_callable(VM *vm, TVal callable, TVal *args, int m);
    TObj *o = (TObj *)self;
    TVal c;
    if (n >= 1 && args[0].t == T_OBJ && ((TObj *)args[0].u.p)->cls == g_task_cls)
        c = tphp_vm_call_callable(vm, args[0], args + 1, n - 1);
    else
        c = tphp_async_spawn(vm, args, n);       /* +1 给调用方 */
    TArr *a = (TArr *)o->props[0].u.p;
    tval_addref(c);                              /* 组再持一份 */
    tarr_append(a, c);
    return c;
}

/* wait(): array —— 等全部完成，按登记序返回结果数组（幂等） */
static TVal nm_tg_wait(VM *vm, void *self, TVal *args, int n) {
    (void)args; (void)n;
    TObj *o = (TObj *)self;
    return tphp_async_all(vm, &o->props[0], 1);
}

static TVal nm_tg_count(VM *vm, void *self, TVal *args, int n) {
    (void)vm; (void)args; (void)n;
    TObj *o = (TObj *)self;
    return TV_INT(((TArr *)o->props[0].u.p)->n);
}

/* ---- Tphp\Task ---- */

/* __invoke(...$args)：启动协程并返回 Coroutine（参数个数上限同 spawn） */
static TVal nm_task_invoke(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    if (n > 8)
        tphp_vm_raise(vm, "ArgumentCountError",
                      "async task supports at most 8 extra arguments");
    TVal buf[9];
    buf[0] = o->props[0];
    for (int i = 0; i < n; i++) buf[i + 1] = args[i];
    return tphp_async_spawn(vm, buf, n + 1);
}

/* async(callable): Tphp\Task —— 内置函数（builtin.c 表项指向这里）。
 * 包装一次、调用任意次；每次调用都新建协程。 */
TVal tphp_async_wrap(VM *vm, TVal *args, int n) {
    if (n != 1 || !tphp_vm_is_callable(args[0]))
        tphp_vm_raise(vm, "TypeError", "async() expects a callable");
    TObj *o = tphp_obj_alloc(g_task_cls);
    o->props[0] = args[0];
    tval_addref(args[0]);
    return TV_OBJ(o);
}

/* ---- 启动/收尾 ---- */

/* 脚本结束时：报告从未被 await/getResult 消费的协程异常 */
static void sched_report_uncaught(void) {
    for (int i = 0; i < g_n_conts; i++) {
        TCont *c = g_conts[i];
        if (c->state != TCS_DONE || !c->exception || c->ex_consumed) continue;
        g_any_uncaught = true;
        VM *vm = tphp_vm_instance();
        const char *file = vm->prog->file ? vm->prog->file : "-";
        fprintf(stderr, "PHP Fatal error:  Uncaught %s: %s in %s:%d"
                        " (in coroutine)\n",
                c->exception->cls->name, tstr_val(c->exception->message),
                file, c->exception->line);
    }
    if (g_any_uncaught) fflush(stdout);
}

static void sched_cleanup(void) {
    for (int i = 0; i < g_n_conts; i++) {
        TCont *c = g_conts[i];
        tval_release(&c->result);
        if (c->exception) {
            TVal exv = TV_OBJ(c->exception);
            tval_release(&exv);
        }
        if (c->coro_obj) {
            TVal ov = TV_OBJ(c->coro_obj);
            tval_release(&ov);        /* 归还 TCont 的那份 */
        }
        tval_release(&c->callable);
        tval_release(&c->chan_val);   /* 挂起在 Channel 队列上未收尾的值 */
        for (int k = 0; k < c->n_start_args; k++)
            tval_release(&c->start_args[k]);
        if (c->cancel_reason) {
            TVal rv = TV_STR(c->cancel_reason);
            tval_release(&rv);
        }
        if (c->fib) tphp_fiber_release(c->fib);
        tm_free(c);
    }
    for (int i = 0; i < g_n_chans; i++) {
        TChan *ch = g_chans[i];
        for (int k = 0; k < ch->len; k++) {
            int idx = (ch->head + k) % (ch->tcap ? ch->tcap : 1);
            tval_release(&ch->buf[idx]);
        }
        tm_free(ch->buf);
        tm_free(ch);
    }
    tm_free(g_chans);
    g_chans = NULL;
    g_n_chans = g_cap_chans = 0;
    tm_free(g_conts);
    g_conts = NULL;
    g_n_conts = g_cap_conts = 0;
    if (g_sched) { tphp_fiber_release(g_sched); g_sched = NULL; }
    g_ready_head = g_ready_tail = g_cur = NULL;
    g_waiting = 0;
    g_any_uncaught = false;
    g_sched_active = false;
    uv_loop_close(&g_loop);           /* 句柄已清空；EBUSY 则随进程退出 */
    g_loop_inited = false;            /* 复位：允许下一次 tphp_sched_run 重建
                                       * loop（pkg fetch 会多次 run_source） */
}

/* 独立 loop 初始化（幂等）：Worker 子线程在 bind async 前调用 */
void tphp_sched_loop_init(void) {
    if (g_loop_inited) return;
    if (uv_loop_init(&g_loop) != 0) {
        fprintf(stderr, "tphp: uv_loop_init failed\n");
        exit(255);
    }
    g_loop_inited = true;
}

int tphp_sched_run(void) {
    tphp_sched_loop_init();
    g_sched_active = true;
    tphp_async_init();
    VM *vm = tphp_vm_instance();
    /* main 协程：接管已准备的 vm->x（main frame；coro_result = NULL） */
    TCont *m = cont_new();
    m->x = vm->x;
    memset(&vm->x, 0, sizeof vm->x);
    vm->x.done = true;
    m->x.coro_result = NULL;          /* main 返回值丢弃 */
    /* main 的 Coroutine 对象惰性创建（tphp_async_current），避免抢占
     * var_dump 的对象编号序列 */
    m->fib = tphp_fiber_new(tphp_fiber_default_stack, main_entry, m);
    sched_ready(m);                   /* fib 就绪后才能入队 */
    /* 调度器 fiber：跑完所有协程后跳回这里 */
    g_sched = tphp_fiber_new(tphp_fiber_default_stack, sched_entry, NULL);
    tphp_fiber_switch(g_sched, NULL);
    sched_report_uncaught();          /* 未消费的协程异常 → fatal */
    int rc = vm->exit_code;
    if (rc == 0 && g_any_uncaught) rc = 255;
    sched_cleanup();
    return rc;
}
