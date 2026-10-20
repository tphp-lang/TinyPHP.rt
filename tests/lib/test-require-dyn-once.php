<?php
// EXPECT: once1
// EXPECT: dyn_once_loaded
// EXPECT: once2
// EXPECT: ok

echo "once1\n";
$p = "helpers/dyn_once.php";
require_once $p;
require_once $p;                      // 运行期 once：只加载一次
echo "once2\n";
echo "ok\n";
