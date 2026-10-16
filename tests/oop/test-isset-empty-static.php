<?php
// 既有缺口随 C1 补齐：isset/empty(C::$x) 与 C::$x op= v（字面 + 动态名）；
// 未声明/不可见/未初始化静默（PHP 对拍）
// EXPECT: i1: 1 0 0
// EXPECT: e1: 1 1
// EXPECT: a1: 11
// EXPECT: a2: 22
// EXPECT: d1: 5
class Z { public static $v = 1; public static $w = 0; private static $pv = 9; }
echo "i1: ", isset(Z::$v) ? "1" : "0", " ", isset(Z::$nope) ? "1" : "0", " ", isset(Z::$pv) ? "1" : "0", "\n";
echo "e1: ", empty(Z::$nope) ? "1" : "0", " ", empty(Z::$w) ? "1" : "0", "\n";
Z::$v += 10;
echo "a1: ", Z::$v, "\n";
$k = "v";
Z::$$k += 11;
echo "a2: ", Z::$v, "\n";
$d = new Z;
Z::$w = 5;
$wk = "w";
echo "d1: ", $d::$$wk, "\n";
