<?php
// EXPECT: start
// EXPECT: side_dyn
// EXPECT: ret=1
// EXPECT: ok

echo "start\n";
$p = "helpers/dyn_marker.php";
$ret = require $p;                    // 动态路径 → OP_REQUIRE
echo "ret=$ret\n";
echo "ok\n";
