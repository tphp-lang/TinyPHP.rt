<?php
// Tphp\Thread（= Tphp\TaskGroup）：任务组 spawn/wait/count，
// 以及"包装一次、调用即启动"的写法
use Tphp\Thread;

function task(int $id, int $ms): int {
    usleep($ms * 100);
    return $id * 10;
}

// 1) Thread：spawn 登记，wait 收口并按登记序返回结果
$g = new Thread();
$g->spawn('task', 1, 300);
$g->spawn('task', 2, 100);
$g->spawn('task', 3, 200);
echo "count=", $g->count(), "\n";
$r = $g->wait();
sort($r);
echo "results=", implode(",", $r), "\n";

// 2) async()：包装一次，之后每次调用都是一次并发启动
$t = async(task(...));
$c = $t(7, 50);
echo "task=", $c->wait(), "\n";

// 3) 任务组也接受 async() 包装出的 Task
$g2 = new Tphp\TaskGroup();
$g2->spawn($t, 4, 20);
$g2->spawn($t, 5, 10);
$r2 = $g2->wait();
sort($r2);
echo "wrapped=", implode(",", $r2), "\n";

// 4) 并发性：3 个 100ms 任务总耗时接近 100ms 而非 300ms
$t0 = microtime(true);
$g3 = new Thread();
for ($i = 1; $i <= 3; $i++) {
    $g3->spawn('task', $i, 1000);        // 1000 * 100us = 100ms
}
$g3->wait();
$ms = (microtime(true) - $t0) * 1000;
echo "concurrent=", $ms < 250 ? "yes" : "no", "\n";

// EXPECT: count=3
// EXPECT: results=10,20,30
// EXPECT: task=70
// EXPECT: wrapped=40,50
// EXPECT: concurrent=yes
