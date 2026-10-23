<?php
// M5-c: Tphp\Worker 基础 —— 真 OS 线程 + mailbox round-trip + stop 退出协议
use Tphp\Worker;

$w = new Worker(function () {
    $me = Worker::self();
    while (($msg = $me->recv()) !== false) {
        $me->post("echo:" . $msg);
    }
});

$w->post("hello");
var_dump($w->recv());
$w->post("world");
var_dump($w->recv());
var_dump($w->id() >= 0, $w->isRunning());
$w->stop();
var_dump($w->recv());        // EOF → false
var_dump($w->isRunning());   // false
echo "worker-done\n";

// EXPECT: string(10) "echo:hello"
// EXPECT: string(10) "echo:world"
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: worker-done
