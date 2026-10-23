<?php
// M5-d 验收（PLAN/02 §6 线程示例适配 + PLAN/04 M5 标准）：
// 4 个 Worker 跑 CPU 密集任务，主线程 sleep 事件循环不受影响；
// 总耗时 ≈ max（并行）而非串行总和
use Tphp\Worker;

function fib($n) { return $n < 2 ? $n : fib($n - 1) + fib($n - 2); }

$workers = [];
for ($i = 0; $i < 4; $i++) {
    $w = new Worker(function () {
        $me = Worker::self();
        while (($job = $me->recv()) !== false) {
            $me->post(fib($job));          // CPU 密集，跑满一核
        }
    });
    $w->on('message', function ($result) {
        echo "got: $result\n";             // stop() drain 时分发
    });
    $workers[] = $w;
}

foreach ($workers as $w) $w->post(26);     // 4 个任务并行
sleep(1);                                  // 主线程 sleep 期间 worker 全速跑
$t0 = microtime(true);                     // 计时只测 drain/join（不含固定 sleep）
foreach ($workers as $w) $w->stop();       // drain：收集 4 个结果并 join
$dt = microtime(true) - $t0;

$t1 = microtime(true);
fib(26);                                   // 主线程单次耗时基准（worker 已 join）
$tSingle = microtime(true) - $t1;

var_dump($dt < $tSingle * 2.5);            // 并行 ≈ max：sleep 期已算完则 dt≈0，
                                           // 否则 ≈ 1×基准；串行需 ≈ 4×基准
echo "workers-done\n";

// EXPECT: got: 121393
// EXPECT: got: 121393
// EXPECT: got: 121393
// EXPECT: got: 121393
// EXPECT: bool(true)
// EXPECT: workers-done
