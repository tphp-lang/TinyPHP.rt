<?php
// EXPECT: start
// EXPECT: got=42
// EXPECT: ok

echo "start\n";
$p = "helpers/ret_marker.php";
$v = require $p;                      // 被包含文件 return 值
echo "got=$v\n";
echo "ok\n";
