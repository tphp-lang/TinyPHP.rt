<?php
// M4-c: 协作式取消 —— 挂起点抛 AsyncCancelled（继承 Throwable，
// catch(Exception) 不误吞）、reason 透传、finally/catch 清理、
// cancel 已完成协程返回 false
use function Tphp\spawn;
use function Tphp\await;
use function Tphp\suspend;

// 1) suspend 挂起点取消 → catch 捕获 + reason
$c1 = spawn(function () {
    try {
        suspend();
        suspend();
        return "never";
    } catch (AsyncCancelled $e) {
        return "cancelled: " . $e->getMessage();
    }
});
suspend();
$c1->cancel("stop-1");
var_dump(await($c1));

// 2) sleep（事件挂起）被取消打断
$c2 = spawn(function () {
    try {
        sleep(10);
        return "never";
    } catch (AsyncCancelled $e) {
        return "caught";
    }
});
suspend();
$c2->cancel();
var_dump(await($c2));

// 3) catch(Exception) 不吞取消；await 透传给主协程
$c3 = spawn(function () {
    try {
        suspend();
        return "never";
    } catch (Exception $e) {
        return "wrong";
    }
});
suspend();
$c3->cancel("no-skip");
try {
    await($c3);
    echo "no\n";
} catch (AsyncCancelled $e) {
    echo "propagated: ", $e->getMessage(), "\n";
}

// 4) finally 清理仍执行
$c4 = spawn(function () {
    try {
        suspend();
        return "never";
    } finally {
        throw new RuntimeException("cleanup-ran");
    }
});
suspend();
$c4->cancel();
try { await($c4); } catch (RuntimeException $e) {
    echo "finally: ", $e->getMessage(), "\n";
}

// 5) cancel 已完成协程 → false
$c5 = spawn(function () { return 1; });
await($c5);
var_dump($c5->cancel());

echo "done\n";

// EXPECT: string(17) "cancelled: stop-1"
// EXPECT: string(6) "caught"
// EXPECT: propagated: no-skip
// EXPECT: finally: cleanup-ran
// EXPECT: bool(false)
// EXPECT: done
