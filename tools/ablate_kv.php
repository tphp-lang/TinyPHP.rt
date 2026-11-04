<?php
// take outline 影响面探针：string 元素读（kv 形态）+ int 元素读（对照）
// 期望（新 outline 版 vs 旧内联版）：string 走 CALL 慢路，观察是否回退
$c = function () {
    $a = [];
    for ($i = 0; $i < 1000; $i++) { $a[] = "k$i"; }
    $x = "";
    for ($r = 0; $r < 100; $r++) {
        for ($i = 0; $i < 1000; $i++) { $x = $a[$i]; }
    }
    return $x;
};
$t0 = microtime(true);
for ($k = 0; $k < 300; $k++) { $c(); }
$t1 = microtime(true);
echo ($t1 - $t0), "\n";
