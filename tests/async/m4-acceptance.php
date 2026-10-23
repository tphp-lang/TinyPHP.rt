<?php
// M4 验收（PLAN/02 §6 异步示例适配 + PLAN/04 M4 验收标准）：
// 透明挂起 / 3 并发耗时≈max / Channel 生产者消费者 / 协作式取消
// （组 use 暂不支持 → 逐条 use）
use function Tphp\spawn;
use function Tphp\await;
use function Tphp\all;

// 1) sleep 透明挂起：spawn 后 main 立即继续
spawn(function () {
    sleep(1);
    echo "woken after 1s\n";
});

// 2) 并发 I/O：2×sleep(2) + 1×https fetch，总耗时 ≈ max 而非串行总和
$t0 = microtime(true);
$r = all([
    spawn(function () { sleep(2); return "s1"; }),
    spawn(function () { sleep(2); return "s2"; }),
    spawn(function () { return http_get("https://example.com/"); }),
]);
$elapsed = microtime(true) - $t0;
var_dump($r[0] === "s1" && $r[1] === "s2"
         && strpos($r[2], "Example Domain") !== false);
var_dump(round($elapsed, 1) < 3.8);        // 串行执行会 ≥ 4s

// 3) Channel：生产者/消费者（PLAN §6 语义，cap=4 触发背压交接）
$ch = new Tphp\Channel(4);
spawn(function () use ($ch) {
    for ($i = 1; $i <= 5; $i++) $ch->send($i * $i);
    $ch->close();
});
while (null !== ($n = $ch->recv())) {
    echo "got $n\n";
}

// 4) 协作式取消（PLAN §6 语义）
$task = spawn(function () { sleep(10); return "done"; });
$task->cancel('no longer needed');
try {
    await($task);
} catch (AsyncCancelled $e) {
    echo "cancelled: {$e->getMessage()}\n";
}

echo "m4-acceptance-done\n";

// EXPECT: woken after 1s
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: got 1
// EXPECT: got 4
// EXPECT: got 9
// EXPECT: got 16
// EXPECT: got 25
// EXPECT: cancelled: no longer needed
// EXPECT: m4-acceptance-done
