<?php
// M4-a: 协程异常 —— await 透传（可捕获）、getResult 透传、
// 未消费异常在脚本结束时报 fatal（stderr，退出码 255）
use function Tphp\spawn;
use function Tphp\await;

// 1) await 重抛，主协程捕获
$c1 = spawn(function () { throw new Exception('boom'); });
try {
    await($c1);
    echo "no\n";
} catch (Exception $e) {
    echo "caught: ", $e->getMessage(), "\n";
}

// 2) 子协程里捕获自己的异常（不外传）
$c2 = spawn(function () {
    try {
        throw new RuntimeException('inner');
    } catch (RuntimeException $e) {
        return 'handled';
    }
});
var_dump(await($c2));

// 3) 异常类型按 catch 匹配（主协程多 catch）
$c3 = spawn(function () { throw new TypeError('bad type'); });
try {
    await($c3);
} catch (Exception $e) {
    echo "wrong branch\n";
} catch (Throwable $e) {
    echo "throwable: ", get_class($e), "\n";
}

// 4) 未消费异常：脚本结束时 fatal（stderr）+ 退出码 255
spawn(function () { throw new Exception('never-handled'); });
echo "main continues\n";

// EXPECT: caught: boom
// EXPECT: string(7) "handled"
// EXPECT: throwable: TypeError
// EXPECT: main continues
// EXPECT-EXIT: 255
