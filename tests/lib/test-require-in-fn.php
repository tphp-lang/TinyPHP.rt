<?php
// EXPECT: enter
// EXPECT: fn_side_effect
// EXPECT: inner=42
// EXPECT: ok

// 函数体内的 require（副作用文件；嵌套函数声明是 PLAN 明确不支持的）
function use_lib() {
    require_once "helpers/fn_marker.php";
    return 21 * 2;
}

echo "enter\n";
$n = use_lib();
echo "inner=$n\n";
echo "ok\n";
