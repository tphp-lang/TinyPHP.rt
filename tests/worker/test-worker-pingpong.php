<?php
// M5-d: ping-pong 1000 —— 单 Worker 1000 次 post/recv round-trip，
// 验证消息序与 mailbox 稳定性（PLAN/04 M5 验收标准）
use Tphp\Worker;

$w = new Worker(function () {
    $me = Worker::self();
    while (($m = $me->recv()) !== false) {
        $me->post($m);                 // 原样回显（含 0）
    }
});

$ok = true;
for ($i = 0; $i < 1000; $i++) {
    $w->post($i);
    if ($w->recv() !== $i) { $ok = false; break; }
}
$w->stop();
var_dump($ok);
echo "ping-pong-done\n";

// EXPECT: bool(true)
// EXPECT: ping-pong-done
