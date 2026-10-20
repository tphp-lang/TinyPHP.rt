<?php
// EXPECT: before
// EXPECT: loaded
// EXPECT: ret=1
// EXPECT: ok

// $v = require 作为赋值 RHS 的表达式形态（返回 1）
function load_val() {
    $v = require_once "helpers/assign_marker.php";
    return $v;
}

echo "before\n";
$n = load_val();
echo "ret=$n\n";
echo "ok\n";
