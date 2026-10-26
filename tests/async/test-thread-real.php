<?php
// Tphp\Thread = 真线程池（OS 线程并行，不是协程）：
//   spawn 的 thunk 闭包作为一次性 Worker 入口（主 Program 字节码跨线程共享）
//   wait() 返回值按 spawn 序；detach() 不等待，线程自己跑完
//   在 FFI 回调（C 栈）里 spawn 也立即执行——协程会饿死，真线程不会
//
// 回归点（P1 修复）：spawn() 返回的 Worker 对象同时被
//   (a) Thread 内部的跟踪数组、(b) 调用方帧（spawn 的返回值）
// 持有。早前两处共用同一份引用计数：wait() 释放跟踪数组时提前 free，
// 调用方帧随后再 release 一次 → 二次释放 → 堆损坏
// （mimalloc: corrupted free list entry / ASAN: heap-use-after-free）。
// 修复后两处各持一份独立引用，本测试覆盖 wait 后再使用返回值的路径。
use Tphp\Thread;

function ttask(int $x): int {
    usleep(50000);
    return $x * 2;
}

// 1) spawn thunk + wait 收集结果
$t = new Thread();
$w1 = $t->spawn(ttask(10));          // parser 改写为 spawn(fn() => ttask(10))
$w2 = $t->spawn(ttask(20));
$r = $t->wait();
sort($r);
echo "wait=", implode(",", $r), "\n";
echo "count-after=", $t->count(), "\n";

// 2) wait 之后仍持有 spawn 返回值：若引用计数算错，这里会读到已释放对象
echo "spawn-obj-alive=", ($w1 !== null && $w2 !== null) ? "yes" : "no", "\n";

// EXPECT: wait=20,40
// EXPECT: count-after=0
// EXPECT: spawn-obj-alive=yes
