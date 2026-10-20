<?php
// EXPECT: start
// EXPECT: after
// EXPECT: miss=
// EXPECT: ok

echo "start\n";
$p = "helpers/no_such_dyn.php";
include $p;                            // include 动态路径失败：不中断
echo "after\n";
$miss = include $p;
echo "miss=$miss\n";                   // false → 空串
echo "ok\n";
