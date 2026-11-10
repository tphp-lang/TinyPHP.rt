<?php
// logstat 百菜第一点：MOD int 快线（TPHP_MOD_FAST）+ tval_mod 边界修复对拍：
// - 快线主路径（双 INT 直出 IR_MOD）与慢线（float/混合）语义一致；
// - PHP_INT_MIN % -1 = 0：HEAD 慢线 x86 idiv #DE（0xC0000094 静默崩，
//   本批 value.c tval_mod 特判 + 快线 -1 分支双侧修复，对齐 PHP）；
// - 除数 0 → DivisionByZeroError（快线显式落既有 slow，逐位原样）；
// - JIT 循环快线面 vs MOD_FAST=0 慢线面同值。
// 值参考 PHP 8.5 同式输出（Deprecated 警告仅 PHP 侧 stderr，不入 EXPECT）。
// 三模逐字一致：默认 / TPHP_MOD_FAST=0 / TPHP_JIT=0。
// EXPECT: --- modfast ---
// EXPECT: a=0 b=1 c=-1 d=-1 e=0 f=2 g=-2
// EXPECT: h=1 i=-1
// EXPECT: z=caught
// EXPECT: k=4 l=0
// EXPECT: END

echo "--- modfast ---\n";

$m = PHP_INT_MIN;
// 1) 边界组（含崩溃修复案 a；负数全象限）
$a = $m % -1;
$b = 7 % -3;
$c = -7 % 3;
$d = -7 % -3;
$e = 0 % 5;
$f = 17 % 5;
$g = $m % 3;
echo "a=$a b=$b c=$c d=$d e=$e f=$f g=$g\n";

// 2) float 慢线（tval_mod 取整语义；PHP 侧此二行出 Deprecated 属预期差异）
$h = 7.9 % 3;
$i = -7.9 % 3;
echo "h=$h i=$i\n";

// 3) 除零 → slow raise（既有语义）
$z = "no-throw";
try {
    $j = 5 % 0;
    $z = "j=$j";
} catch (DivisionByZeroError $err) {
    $z = "caught";
}
echo "z=$z\n";

// 4) JIT 循环快线面：const 除数 + 变量除数（双快线输入形态）
$k = 0;
$l = 0;
$dv = 9;
for ($t = 0; $t < 100000; $t++) {
    $k = $t % 7;     // const 除数
    $l = $t % $dv;   // 变量除数
}
echo "k=$k l=$l\n";

echo "END\n";
