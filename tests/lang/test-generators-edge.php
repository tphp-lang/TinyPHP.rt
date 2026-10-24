<?php
// EXPECT: rewind-err
// EXPECT: getreturn-err
// EXPECT: closed-foreach-err
// EXPECT: closed-next-silent=ok
// EXPECT: closed-current=null
// EXPECT: running-err
// EXPECT: ok
// G3: generator edge cases (PHP parity)

function g1() { yield 1; }

// 1) rewind 已运行过 → Error
try {
    $g = g1();
    $g->next();
    $g->rewind();
    echo "rewind-FAIL\n";
} catch (Error $e) {
    echo "rewind-err\n";
}

// 2) getReturn 未返回 → Error
try {
    $g = g1();
    $g->current();          // 初始化到首个 yield，未 return
    $g->getReturn();
    echo "getreturn-FAIL\n";
} catch (Error $e) {
    echo "getreturn-err\n";
}

// 3) 已关闭（耗尽）生成器 foreach → Error（同一对象耗尽后再遍历）
try {
    $g3 = g1();
    foreach ($g3 as $v) { /* 耗尽 */ }
    foreach ($g3 as $v) { /* 已关闭 */ }
    echo "closed-foreach-FAIL\n";
} catch (Error $e) {
    echo "closed-foreach-err\n";
}

// 4) 已关闭 next/send 静默无操作
function g2() { yield 1; return; }
$g = g2();
foreach (g2() as $v) {}
$x = g2();
$x->next();                 // 耗尽
$x->next();                 // 静默
echo "closed-next-silent=ok\n";

// 5) 已关闭 current = null
$y = g2();
$y->next();                 // init→yield
$y->next();                 // 关闭
echo "closed-current=", ($y->current() === null ? "null" : "?"), "\n";

// 6) RUNNING 重入 → Error（next 两步语义：init+推进，重入发生在推进段——
//    外层 next 也须在 try 内）
$w = new class { public $g; };
$gen = function () use ($w) {
    yield 1;
    $w->g->next();          // 在生成器内 resume 自己 → 重入
};
$w->g = $gen();
try {
    $w->g->next();          // init→yield1 后立即推进 → 体内 next → RUNNING
    echo "running-FAIL\n";
} catch (Error $e) {
    echo "running-err\n";
}
echo "ok\n";
