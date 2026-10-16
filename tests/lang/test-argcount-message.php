<?php
// B8: ArgumentCountError 消息尾巴（PHP 8 对拍：", N passed in FILE on line L
// and (exactly|at least) M expected"；文件名路径因运行目录而异，截除之）
// EXPECT: Too few arguments to function f1(), 0 passed| on line 9 and exactly 1 expected
// EXPECT: line=8
// EXPECT: Too few arguments to function g(), 0 passed| on line 16 and at least 1 expected
// EXPECT: line=15
function f1(int $a) { return $a; }
try { f1(); } catch (Throwable $e) {
    $m = $e->getMessage();
    $p = strpos($m, "passed in ");
    echo substr($m, 0, $p + 6), "|", substr($m, strrpos($m, " on line ")), "\n";
    echo "line=", $e->getLine(), "\n";
}
function g($a, $b = 2) { return $a; }
try { g(); } catch (Throwable $e) {
    $m = $e->getMessage();
    $p = strpos($m, "passed in ");
    echo substr($m, 0, $p + 6), "|", substr($m, strrpos($m, " on line ")), "\n";
    echo "line=", $e->getLine(), "\n";
}
