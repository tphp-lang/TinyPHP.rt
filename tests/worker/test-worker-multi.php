<?php
// M5-c: 多 Worker 并发 —— 3 个线程并行回声；主线程 sleep/IO 不被阻塞
// （worker 满载时主线程事件循环照常推进，PLAN/04 验收语义）
use tphp\Worker;
use function Async\spawn;
use function Async\await;

$mk = function () {
    return new Worker(function () {
        $me = Worker::self();
        while (($m = $me->recv()) !== false) {
            $me->post($m * 2);
        }
    });
};
$w1 = $mk();
$w2 = $mk();
$w3 = $mk();

// 主线程并发：一个协程 sleep，同时给 3 个 worker 发任务
$main_io = spawn(function () {
    sleep(1);                          // 挂起，不阻塞
    return "io-ok";
});

$w1->post(1);
$w2->post(2);
$w3->post(3);
var_dump($w1->recv(), $w2->recv(), $w3->recv());
var_dump(await($main_io));

$w1->stop();
$w2->stop();
$w3->stop();
echo "multi-done\n";

// EXPECT: int(2)
// EXPECT: int(4)
// EXPECT: int(6)
// EXPECT: string(5) "io-ok"
// EXPECT: multi-done
