<?php
// A2 位运算刀（2026-11-14）：TPHP_BW_FAST=1 JIT BW_AND/BW_OR/BW_XOR/SHL/SHR
// INT 快线（原恒 call_slow_binop ~28ns/次；ternary 剖面 20× 实锤）。
//   AND/OR/XOR int64 全定义直出；SHL/SHR shift 值 (uint64)<=63 守卫，
//   负 shift（ValueError）与 >=64（慢线常数语义）落 slow 原行为。
// 三模语义：默认 / BW_FAST=0 / JIT=0 输出须与 EXPECT 逐字一致，
// EXPECT 取自 PHP 8.5 对拍（php-8.5.1-nts 双端 diff 逐字一致）。

// c1 AND/OR/XOR：正/负/0/INT64 边界（字面量 CONST 操作数）
$a = 0xFF & 0x0F;
$b = 0xF0 | 0x0F;
$c = 0xFF ^ 0x0F;
$d = -12 & 10;
$e = -12 | 10;
$f = -12 ^ 10;
$g = PHP_INT_MAX & -1;
$h = PHP_INT_MIN | 0;
$i2 = PHP_INT_MIN ^ -1;
echo "c1 $a $b $c $d $e $f $g $h $i2\n";
// EXPECT: c1 15 255 240 0 -2 -2 9223372036854775807 -9223372036854775808 9223372036854775807

// c2 热形态：ternary + $i&1（剖面 20× 元凶形态，JIT 快线主战场）
$s = 0;
for ($i = 0; $i < 10; $i++) { $s += ($i & 1) ? 1 : 2; }
echo "c2 $s\n";
// EXPECT: c2 15

// c3 SHL/SHR 常量 shift：0/1/63/64 边界（64 = 慢线 s>=64 语义）
$p = 1 << 0;
$q = 1 << 6;
$r = 1 << 63;
$u = 1 << 64;
$v2 = -1 >> 63;
$w2 = -1 >> 64;
$x2 = 5 >> 1;
echo "c3 $p $q $r $u $v2 $w2 $x2\n";
// EXPECT: c3 1 64 -9223372036854775808 0 -1 -1 2

// c4 动态 shift（JIT 拒编走解释器，语义同 slow）+ 负 shift 异常
// 注记：负 shift 异常类差异（既有，非本刀面）——tphp=ValueError、
// PHP 8=ArithmeticError（11-14 对拍实锤，记 backlog P1 语义缺口批）；
// 本测试按 tphp 语义锁定，echo 不含类名保双端可比。
$sh = 3;
echo "c4 ", 7 << $sh, " ", 7 >> $sh, "\n";
// EXPECT: c4 56 0
try { $bad = 1 << -1; echo "c4 no-raise\n"; }
catch (Throwable $e) { echo "c4 shift-neg raised\n"; }
// EXPECT: c4 shift-neg raised

// c5 非 INT 操作数落 slow（字符串数字 to_int；float 案移除——PHP 8.5 打
// float→int 隐式转换 Deprecated 而 tphp 无诊断通道（B1 翻案域既有差异，
// 11-14 对拍实锤记档），BW 语义本身双端一致 5.9&3=1）
echo "c5 ", "5" & 3, " ", "7" | 8, "\n";
// EXPECT: c5 1 15

// c6 dst 别名（$x = $x & mask 覆盖写）
$x = 0b1100;
$x = $x & 0b1010;
$y = 0b1100;
$y = $y | 0b0011;
$z = 0b1100;
$z = $z ^ 0b0110;
echo "c6 $x $y $z\n";
// EXPECT: c6 8 15 10

// c7 混合循环：OR/XOR/SHL 动态累计
$s = 0;
$acc = 1;
for ($i = 0; $i < 8; $i++) {
    $acc = ($acc | 1) ^ (1 << ($i & 3));
    $s += $acc & 7;
}
echo "c7 $s $acc\n";
// EXPECT: c7 30 1

// c8 XOR 交换律对拍 + 与/或组合（INT64_MIN 边界）
$m = PHP_INT_MIN;
$n = $m & -1;
$o = ($n ^ $m) & 1;
echo "c8 $n $o\n";
// EXPECT: c8 -9223372036854775808 0
