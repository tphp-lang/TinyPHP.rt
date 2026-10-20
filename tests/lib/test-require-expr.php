<?php
// EXPECT: before
// EXPECT: ret=1
// EXPECT: lib=42
// EXPECT: ok

echo "before\n";
$ret = require "helpers/util.php";
echo "ret=$ret\n";
echo "lib=", lib_double(21), "\n";   // 内联仍生效
echo "ok\n";
