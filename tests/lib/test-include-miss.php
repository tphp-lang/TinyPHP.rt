<?php
// EXPECT: start
// EXPECT: after
// EXPECT: missing=
// EXPECT: ok

echo "start\n";
include "helpers/no_such_file.php";   // include 失败：静默跳过，不中断
echo "after\n";
$missing = include "helpers/no_such_file.php";
echo "missing=$missing\n";            // 表达式形态返回 false → 插值为空串
echo "ok\n";
