<?php
// EXPECT: first
// EXPECT: inc_once_loaded
// EXPECT: second
// EXPECT: once_return=1
// EXPECT: ok

echo "first\n";
require_once "helpers/once_marker.php";
echo "second\n";
require_once "helpers/once_marker.php";   // 已加载：不再输出
$rv = require_once "helpers/once_marker.php"; // 表达式形态，仍返回 1
echo "once_return=$rv\n";
echo "ok\n";
