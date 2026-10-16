<?php
// B2: 闭包/方法内的 global（顶层从未出现的名走动态全局条目；顶层出现则共享
// main 槽——与 PHP 对拍 q2/q2b）
// EXPECT: B2 closure: 5
// EXPECT: top: 5
// EXPECT: B2 method: 7
$f = function() { global $g2; $g2 = 5; return $g2; };
echo "B2 closure: ", $f(), "\n";
echo "top: ", $g2, "\n";
class C2 { function m() { global $g3; $g3 = 7; return $g3; } }
echo "B2 method: ", (new C2)->m(), "\n";
