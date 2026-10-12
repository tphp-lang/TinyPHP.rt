<?php
// M4-a: Async\spawn / await 基础 —— 返回值、实参、命名函数、Coroutine 对象
use function Async\spawn;
use function Async\await;

$c1 = spawn(function () { return 42; });
var_dump(await($c1));

$c2 = spawn(function ($a, $b) { return $a + $b; }, 20, 22);
var_dump(await($c2));

function worker($n) { return "worker-$n"; }
var_dump(await(spawn('worker', 7)));

// Coroutine 对象方法
$c3 = spawn(function () { return 'r3'; });
var_dump($c3->isDone());            // false（尚未跑完——spawn 只入队）
var_dump(await($c3));
var_dump($c3->isDone());            // true
var_dump($c3->getResult());         // 可重复取
var_dump($c3->getException());      // null

// await 已完成协程：直接取值
var_dump(await($c3));

// instanceof 与类型
var_dump($c3 instanceof Async\Coroutine);

// await 非协程 → TypeError（fatal，见 EXPECT-EXIT）
$x = spawn(function () { return 1; });
try {
    await('nope');
} catch (Throwable $e) {
    echo "caught: ", get_class($e), "\n";
}

// closure 捕获外部变量
$f = 100;
$c4 = spawn(function () use ($f) { return $f + 1; });
var_dump(await($c4));

// EXPECT: int(42)
// EXPECT: int(42)
// EXPECT: string(8) "worker-7"
// EXPECT: bool(false)
// EXPECT: string(2) "r3"
// EXPECT: bool(true)
// EXPECT: string(2) "r3"
// EXPECT: NULL
// EXPECT: string(2) "r3"
// EXPECT: bool(true)
// EXPECT: caught: TypeError
// EXPECT: int(101)
