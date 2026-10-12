<?php
// M4-c: Async\all / any / current / run
use function Async\spawn;
use function Async\await;
use function Async\all;
use function Async\any;
use function Async\current;
use function Async\run;

// 1) all：按输入序返回（含不同返回类型）
$cs = [
    spawn(function () { return 1; }),
    spawn(function () { return "two"; }),
    spawn(function () { sleep(0); return 3.5; }),
];
var_dump(all($cs));

// 2) any：首个成功者
$any = [
    spawn(function () { return "fast"; }),
    spawn(function () { sleep(0); return "slow"; }),
];
var_dump(any($any));

// 3) any 全败 → 抛最后一个异常
$allbad = [
    spawn(function () { throw new Exception("e1"); }),
    spawn(function () { sleep(0); throw new Exception("e2"); }),
];
try { any($allbad); } catch (Throwable $e) { echo "any: ", $e->getMessage(), "\n"; }

// 4) all 遇异常 → 透传
$badmix = [
    spawn(function () { return "ok"; }),
    spawn(function () { sleep(0); throw new RuntimeException("boom"); }),
];
try { all($badmix); } catch (Throwable $e) { echo "all: ", $e->getMessage(), "\n"; }

// 5) current：协程内拿到自己的 Coroutine 对象
$c = spawn(function () { return current()->isRunning(); });
var_dump(await($c));

// 6) run(callable)：等价 spawn + await，返回 void（PLAN 签名）
var_dump(run(function () { return "via-run"; }));

echo "done\n";

// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   string(3) "two"
// EXPECT:   [2]=>
// EXPECT:   float(3.5)
// EXPECT: }
// EXPECT: string(4) "fast"
// EXPECT: any: e2
// EXPECT: all: boom
// EXPECT: bool(true)
// EXPECT: NULL
// EXPECT: done
