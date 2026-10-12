/*
 * TinyPHP.rt - sched.h - M4 协程调度器（fiber + libuv 事件循环）
 *
 * 把脚本变成 main 协程；Async\spawn 出的协程由调度器 fiber 驱动，
 * 全部完成后返回退出码。挂起（sleep/await/suspend/无感 I/O）都落在
 * tphp_fiber_suspend()——builtin 深处冻结 C 栈，事件回调唤醒。
 */
#ifndef TPHP_SCHED_H
#define TPHP_SCHED_H

#include "opcode.h"

typedef struct TCont TCont;

/* 启动调度器并运行到所有协程完成（tphp_vm_run 调用）。返回退出码：
 * main 协程未捕获异常 → 255；子协程有未捕获异常 → 255。 */
int tphp_sched_run(void);

/* 内部类注册（Async\Coroutine）：必须在 tphp_compile 之前调用
 * （A_NEW 做编译期类存在性检查）；幂等。 */
void tphp_async_init(void);

/* ---- 通用事件挂起（M4-b I/O 等 uv 回调驱动的事件）----
 * 等待者挂起 → uv 回调 fire → 唤醒。事件结构公开字段由调用方持有
 * （可放协程栈上：等待期间栈冻结，回调只写 fired/ud）。 */
typedef struct SchedEvent {
    bool fired;            /* 事件已触发（wait 前已 fire 则直接返回） */
    void *ud;              /* 调用方自用的结果槽 */
    void *waiter;          /* TCont*（opaque，sched.c 内部使用） */
} SchedEvent;

void tphp_sched_event_reset(SchedEvent *ev);   /* 复位（复用前调用） */
void tphp_sched_event_wait(SchedEvent *ev);    /* 挂起当前协程直至 fire */
void tphp_sched_event_fire(SchedEvent *ev);    /* uv 回调里触发并唤醒 */

/* ---- 细粒度挂起原语（net.c 的 TCP 读/accept 等）----
 * 惯用序列：wait_begin() → [登记等待者] → wait_suspend() →
 * [解除登记] → check_cancel()。唤醒方用 wake(c)。 */
TCont *tphp_sched_current(void);      /* 当前协程（宿主线程上为 NULL） */
void tphp_sched_ready(TCont *c);      /* 仅入就绪队列（不改 g_waiting） */
void tphp_sched_wake(TCont *c);       /* g_waiting-- + 入就绪队列 */
void tphp_sched_wait_begin(void);     /* 记录挂起位置 + g_waiting++ */
void tphp_sched_wait_suspend(void);   /* 挂起（WAITING） */
void tphp_sched_check_cancel(void);   /* 恢复点：被 cancel 则抛 AsyncCancelled */

/* 事件循环句柄与活跃状态（io.c 发起 uv 操作用） */
struct uv_loop_s;
struct uv_loop_s *tphp_sched_loop(void);
bool tphp_sched_active(void);
void tphp_sched_loop_init(void);   /* 幂等 loop init（Worker 子线程用） */

/* Async\ 内置（注册进 builtin 表；名字走命名空间解析） */
TVal tphp_async_spawn(VM *vm, TVal *args, int n);     /* spawn(callable, ...) */
TVal tphp_async_await(VM *vm, TVal *args, int n);     /* await(Coroutine)     */
TVal tphp_async_suspend(VM *vm, TVal *args, int n);   /* suspend(): 让出一次  */
TVal tphp_async_sleep(VM *vm, TVal *args, int n);     /* sleep($sec)          */
TVal tphp_async_usleep(VM *vm, TVal *args, int n);    /* usleep($usec)        */
TVal tphp_async_current(VM *vm, TVal *args, int n);   /* current(): Coroutine */
TVal tphp_async_all(VM *vm, TVal *args, int n);       /* all(array): array    */
TVal tphp_async_any(VM *vm, TVal *args, int n);       /* any(array): mixed    */
TVal tphp_async_run(VM *vm, TVal *args, int n);       /* run(callable): void  */

#endif /* TPHP_SCHED_H */
