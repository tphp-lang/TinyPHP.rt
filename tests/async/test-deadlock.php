<?php
// M4: 死锁检测——全体协程挂起且无待定事件 → DeadlockError fatal（退出码 255）
// （挂起位置报告：coroutine #N suspended in fn:line）
use function Tphp\spawn;
use function Tphp\await;

$f = spawn(function () {
    $ch = new Tphp\Channel(1);   // 永远没有 send，recv 挂起成死锁
    return $ch->recv();
});
var_dump(await($f));
echo "never reached\n";

// fatal 报告走 stderr（EXPECT 协议只对 stdout）：
//   PHP Fatal error: Uncaught DeadlockError: 2 coroutine(s) suspended with no pending events
//     coroutine #0 suspended in ~main:11
//     coroutine #1 suspended in {closure}:9
// stdout 无输出，仅验证退出码
// EXPECT-EXIT: 255
