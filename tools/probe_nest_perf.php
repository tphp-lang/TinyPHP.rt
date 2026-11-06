<?php
// 刀5C 性能探针：长循环 + 嵌套调用（NEST_CALL=0 时整函数 FAILED → 解释器）。
// 同二进制 env 切换 A/B，min-of-6 交替。
function loopnest($n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) $s += strlen(trim("  x$i  "));
    return $s;
}
$t0 = microtime(true);
$r = loopnest(200000);
$t1 = microtime(true);
echo "BENCH loop-nest ", round(($t1 - $t0) * 1000, 1), " ms sum=$r\n";
