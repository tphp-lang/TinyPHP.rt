<?php
// NEG 面刀（2026-11-13 object 刀）：
//   TPHP_NEG_FOLD=1 编译期一元负号字面量折叠（`-1` → CONST -1，免 OP_NEG）
//   TPHP_NEG_FAST=1 JIT NEG INT 快线（tag==INT 且 pay!=INT64_MIN → IR 直取负，
//     否则落既有 slow；开关关 = HEAD 恒 call_slow_binop）
// 三模语义：默认 / NEG_FOLD=0 / NEG_FAST=0 / JIT=0 输出须与 EXPECT 逐字一致，
// EXPECT 取自 PHP 8.5 对拍。

// c1 字面量 int 折叠（含嵌套/括号）
$a = -1;
$b = -(-2);
$c = -(3);
echo "c1 $a $b $c\n";
// EXPECT: c1 -1 2 -3

// c2 字面量 float 折叠
$f = -1.5;
$g = -2.25;
echo "c2 $f $g\n";
// EXPECT: c2 -1.5 -2.25

// c3 动态 int 取负 + dst 别名（$i = -$i 覆盖写）
$s = 0;
for ($i = 0; $i < 5; $i++) {
    $v = -$i;
    $s += $v;
}
$x = 5;
$x = -$x;
$x = -$x;
echo "c3 $s $x\n";
// EXPECT: c3 -10 5

// c4 动态 float 取负
$d = -3.5;
$d = -$d;
$d = -$d;
echo "c4 $d\n";
// EXPECT: c4 -3.5

// c5 字符串数字取负（非 INT 走 slow 线语义）；非数字串 = TypeError（PHP 8 语义）
echo "c5 ", -"5", " ", -"0", "\n";
// EXPECT: c5 -5 0
try {
    $bad = -"abc";
    echo "c5x $bad\n";
} catch (TypeError $e) {
    echo "c5x caught\n";
}
// EXPECT: c5x caught
// 既有差异记档（非本刀面，HEAD/JIT=0 同）：tphp value.c to_num 无 bool/null
// 分支 → -true/-null 抛 TypeError；PHP 强制转换得 -1/0。此处按 tphp 语义锁定。
try {
    $z = -true;
    echo "c5b $z\n";
} catch (TypeError $e) {
    echo "c5b caught-tphp\n";
}
// EXPECT: c5b caught-tphp
try {
    $z = -null;
    echo "c5n $z\n";
} catch (TypeError $e) {
    echo "c5n caught-tphp\n";
}
// EXPECT: c5n caught-tphp

// c6 INT64_MIN 边界：动态（恒 float）与字面量形态
$m = -9223372036854775807 - 1;
$n = -$m;
$lit = -9223372036854775808;
echo "c6 $n $lit\n";
// EXPECT: c6 9.2233720368548E+18 -9.2233720368548E+18

// c7 -PHP_INT_MIN（A_CONSTREF 不折，运行期 slow 线）
$p = -PHP_INT_MIN;
echo "c7 $p\n";
// EXPECT: c7 9.2233720368548E+18

// c8 折叠在表达式/实参位（CONST -2 直入）
echo "c8 ", 10 - -3, " ", substr("hello", -2), " ", str_repeat("x", 4 - -1), "\n";
// EXPECT: c8 13 lo xxxxx

// c9 dst 旧值为字符串的覆盖写（释放门）+ 500 轮稳定
$t = "abc";
for ($k = 0; $k < 500; $k++) {
    $t = -$k;
}
echo "c9 $t\n";
// EXPECT: c9 -499

// c10 数组读 + 动态取负喂累加
$arr = [1, 2, 3];
$ss = 0;
foreach ($arr as $v) {
    $ss += -$v;
}
echo "c10 $ss\n";
// EXPECT: c10 -6
