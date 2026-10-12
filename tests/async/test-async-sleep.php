<?php
// M4-a: sleep/usleep 透明挂起 —— 事件循环调度，多协程交错执行
// （本测试断言确定性交错顺序，不依赖墙钟）
use function Async\spawn;
use function Async\await;
use function Async\suspend;

// 1) suspend 协作式让出：两个协程交错
spawn(function () { echo "a1\n"; suspend(); echo "a2\n"; });
spawn(function () { echo "b1\n"; suspend(); echo "b2\n"; });

// 2) sleep 不阻塞其他协程：完成顺序由调度器决定（c3 即时，c1 零延时，c2 1ms）
$c1 = spawn(function () { sleep(0); return 'c1'; });
$c2 = spawn(function () { usleep(1000); return 'c2'; });
$c3 = spawn(function () { return 'c3'; });
var_dump(await($c1));
var_dump(await($c2));
var_dump(await($c3));

// EXPECT: a1
// EXPECT: b1
// EXPECT: a2
// EXPECT: b2
// EXPECT: string(2) "c1"
// EXPECT: string(2) "c2"
// EXPECT: string(2) "c3"
