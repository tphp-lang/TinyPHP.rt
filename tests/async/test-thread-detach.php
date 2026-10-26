<?php
// Tphp\Thread::detach() 回归：fire-and-forget —— 线程自己跑完，父侧不等待。
// 覆盖 P1 修复的两处：
//   1. detach 把 Worker 引用移交孤儿表（引用计数必须正确转移，不能重复释放）
//   2. detach 后立即退出主脚本时，后台线程的收尾不能破坏堆
//      （早前 detach 的 Worker 对象被提前释放 → mimalloc 堆校验失败）
use Tphp\Thread;

// 1) detach 立即返回（不等待线程完成）
$g = new Thread();
$g->spawn(function () { usleep(80000); });
$t0 = microtime(true);
$g->detach();
$ms = (microtime(true) - $t0) * 1000;
echo "detach=", $ms < 50 ? "instant" : "slow", "\n";

// 2) detach 后主线程继续跑，后台线程同时在跑（真实并发窗口）
for ($i = 0; $i < 20; $i++) {
    $x = new Thread();
    $x->spawn(function () { usleep(2000); });
    $x->detach();
}
echo "loop-done\n";

// EXPECT: detach=instant
// EXPECT: loop-done
