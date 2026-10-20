<?php
// EXPECT: start
// EXPECT: fn=99
// EXPECT: ok

echo "start\n";
$p = "helpers/dyn_lib.php";
require $p;                            // 动态路径 → 运行期加载
echo "fn=", dyn_triple(33), "\n";     // 函数在 inc_progs 可解析
echo "ok\n";
