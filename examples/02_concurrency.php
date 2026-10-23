<?php
/**
 * TinyPHP.rt 并发编程推荐风格：无色优先，显式分叉
 *
 * 五条原则：
 *   1. 业务函数不标记 —— 普通 function，内部 IO 自动挂起
 *   2. 并发只在调用点 —— spawn 是唯一分叉点，函数本身不知道自己是串行还是并发
 *   3. 传函数用 first-class callable —— f(...) 而不是 'f'
 *   4. 谁 spawn 谁收口 —— 同作用域内 all/await，不留孤儿协程
 *   5. 错误就近 catch —— 异常在 await 点抛出，与同步代码完全一致
 *
 * 运行：build/tphp run examples/02_concurrency.php
 */

use function Tphp\{spawn, await, all, async};
use Tphp\Thread;
use tphp\Worker;

// ─────────────────────────────────────────────────────────────
// 1. 业务函数：无色。不写 async，不写 await。
//    真实场景把 usleep 换成 http_get / file_get_contents / fsockopen。
// ─────────────────────────────────────────────────────────────
function fetchUser(int $id): array
{
    usleep(50000);                          // 注意：sleep() 单位是秒，毫秒用 usleep
    if ($id <= 0) {
        throw new InvalidArgumentException("invalid user id: $id");
    }
    return ['id' => $id, 'name' => "user$id"];
}

function fetchOrders(int $uid): array
{
    usleep(30000);
    return ["orderA($uid)", "orderB($uid)"];
}

// ─────────────────────────────────────────────────────────────
// 2. 串行：直接调用。没有 spawn，没有 await。
//    当前协程在 usleep 处自动挂起，事件循环继续跑别的协程。
// ─────────────────────────────────────────────────────────────
function demoSerial(): void
{
    $user   = fetchUser(2);                 // 返回就是数组，不是 Promise
    $orders = fetchOrders($user['id']);
    echo "serial   : ", $user['name'], " ", implode(",", $orders), "\n";
}

// ─────────────────────────────────────────────────────────────
// 3. 并发：spawn 是唯一的显式分叉点。
//    first-class callable 避免字符串函数名，参数直接跟在后面。
//    all() 按输入序返回，任一出错即抛出。
// ─────────────────────────────────────────────────────────────
function demoParallel(): void
{
    $t0 = microtime(true);
    [$user, $orders] = all([
        spawn(fetchUser(...), 3),
        spawn(fetchOrders(...), 3),
    ]);
    $ms = (microtime(true) - $t0) * 1000;
    echo "parallel : ", $user['name'], " ", implode(",", $orders),
         " ", sprintf("%.0f", $ms), "ms\n";   // ≈ max(50,30)，不是 80
}

// ─────────────────────────────────────────────────────────────
// 4. 错误：spawn 出去的异常在 await/all 处抛出，try/catch 照常。
//    不要 spawn 后不管——没有 await 的协程，异常会静默丢失。
// ─────────────────────────────────────────────────────────────
function demoError(): void
{
    try {
        all([spawn(fetchUser(...), -1)]);
    } catch (InvalidArgumentException $e) {
        echo "caught   : ", $e->getMessage(), "\n";
    }
}

// ─────────────────────────────────────────────────────────────
// 5. 取消：拿到 Coroutine 就能取消，await 处抛 AsyncCancelled。
//    适合超时、用户中断、上游失败等场景。
// ─────────────────────────────────────────────────────────────
function demoCancel(): void
{
    $c = spawn(function () {
        usleep(10000000);                   // 模拟一个很慢的任务
        return "never";
    });
    $c->cancel('deadline exceeded');
    try {
        await($c);
    } catch (AsyncCancelled $e) {
        echo "cancelled: ", $e->getMessage(), "\n";
    }
}

// ─────────────────────────────────────────────────────────────
// 6. Channel：协程间通信。cap 满时 send 自动挂起，天然背压。
//    多消费者/多生产者都支持（等待者按 FIFO 唤醒）。
// ─────────────────────────────────────────────────────────────
function demoChannel(): void
{
    $ch = new Tphp\Channel(4);
    spawn(function () use ($ch) {
        for ($i = 1; $i <= 3; $i++) {
            $ch->send($i * $i);
        }
        $ch->close();
    });

    $got = [];
    while (null !== ($n = $ch->recv())) {   // null = 通道已关闭
        $got[] = $n;
    }
    echo "channel  : ", implode(",", $got), "\n";
}

// ─────────────────────────────────────────────────────────────
// 7. Worker：真 OS 线程 + 独立 VM。
//    handler 是一个"消息进、消息出"的循环；闭包必须定义在主脚本里
//    （不能来自 autoload 的库文件），这是当前的硬约束。
//    约定：消息用数组/对象，不要用裸 false —— recv() 用 false 表示 EOF。
// ─────────────────────────────────────────────────────────────
function demoWorker(): void
{
    $w = new Worker(function () {
        $me = Worker::self();
        while (($msg = $me->recv()) !== false) {
            $me->post(['result' => $msg['n'] * 2]);
        }
    });

    $w->post(['n' => 21]);
    $reply = $w->recv();
    echo "worker   : ", $reply['result'], "\n";
    $w->stop();
}

// ─────────────────────────────────────────────────────────────
// 8. 组合：并发批处理 + 限流。
//    Channel 支持多等待者，可以直接当信号量用：预置 N 个令牌，
//    每个任务先取令牌再干活，finally 归还。同时最多 N 个在飞。
// ─────────────────────────────────────────────────────────────
function demoThrottle(): void
{
    $ids   = [1, 2, 3, 4, 5, 6];
    $limit = 2;                             // 同时最多 2 个在飞
    $sem   = new Tphp\Channel($limit);
    for ($i = 0; $i < $limit; $i++) {
        $sem->send(true);                   // 预置令牌
    }

    $tasks = [];
    foreach ($ids as $id) {
        $tasks[] = spawn(function () use ($id, $sem) {
            $sem->recv();                   // 取令牌；无令牌则排队等待
            try {
                return fetchUser($id)['name'];
            } finally {
                $sem->send(true);           // 归还令牌
            }
        });
    }

    echo "throttle : ", implode(",", all($tasks)), "\n";
}

// ─────────────────────────────────────────────────────────────
// 9. 任务组：new Thread()（= Tphp\TaskGroup）+ spawn + wait。
//    与 all([...]) 等价，但更适合"分散启动、统一收口"的流程。
//    wait() 按登记序返回结果数组，可重复调用（幂等）。
// ─────────────────────────────────────────────────────────────
function demoGroup(): void
{
    $g = new Thread();
    $g->spawn(fetchUser(...), 4);
    $g->spawn(fetchOrders(...), 4);
    [$user, $orders] = $g->wait();
    echo "group    : ", $user['name'], " ", implode(",", $orders), "\n";
}

// ─────────────────────────────────────────────────────────────
// 10. async(EXPR)：把"调用表达式"整体搬进协程。
//     async(p1(3,4)) 里的 p1 不会先执行——parser 改写成
//     spawn(fn() => p1(3,4))，自由变量按 spawn 那一刻捕获。
//     单实参不是调用形态时（async($callable)）退化为"包装成任务工厂"。
// ─────────────────────────────────────────────────────────────
function demoTask(): void
{
    // 形态 A：直接写调用，async 负责搬进协程
    async(fetchUser(5));                          // 启动即走，不阻塞
    echo "async    : ", await(async(fetchUser(6)))['name'], "\n";

    // 形态 B：包装成可复用的任务工厂，调用即启动
    $fetchUserAsync = async(fetchUser(...));
    $c1 = $fetchUserAsync(7);
    $c2 = $fetchUserAsync(8);
    echo "task     : ", $c1->wait()['name'], " ", $c2->wait()['name'], "\n";
}

// ─────────────────────────────────────────────────────────────
// 主流程：无色模型下，main 本身就是协程，直接顺序调用即可。
// ─────────────────────────────────────────────────────────────
demoSerial();
demoParallel();
demoError();
demoCancel();
demoChannel();
demoWorker();
demoThrottle();
demoGroup();
demoTask();
echo "done\n";
