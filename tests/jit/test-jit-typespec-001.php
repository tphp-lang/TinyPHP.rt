<?php
// 类型特化批1（TPHP_TYPE_SPEC：preheader guard + 三检查点跳过）语义对拍矩阵：
// - 特化主路径：int 元素 × int 累加器（guard 通过 → 检查点跳过）；
// - guard-fail 路径：float/str/混合元素、非 packed（unset 洞）、acc 入口
//   非 INT、编译后类型轮转（每调用一次 RETURN 重跑）；
// - 溢出 RETURN 安全链：特化体内 int64 溢出 → RETURN(0) → 解释器整函数
//   重跑 → double 结果（绝不落 FLOAT store 破坏假设）；
// - DEC 倒序、空数组（n=0 全INT 扫描不触 data）。
// 三模逐字一致：默认 / TPHP_TYPE_SPEC=0 / TPHP_JIT=0。
// EXPECT: --- typespec ---
// EXPECT: it=55 integer
// EXPECT: st=6 integer
// EXPECT: mx=6.5 double
// EXPECT: fs=6.5 double
// EXPECT: np=2 integer
// EXPECT: of=9.2233720368548E+18 double
// EXPECT: dec=55 integer
// EXPECT: emp=0 integer
// EXPECT: rt=6 integer
// EXPECT: END

echo "--- typespec ---\n";

// 1) 特化主形态（guard pass）：$s += $a[$i]; $i++
$fi = function ($a, $n) {
    $s = 0;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$ai = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
$v = 0;
for ($t = 0; $t < 200; $t++) $v = $fi($ai, 10);
echo "it=$v " . gettype($v) . "\n";

// 2) str 元素（guard fail：全INT 扫描不过）→ 解释器累加
$st = function ($a, $n) {
    $s = 0;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$astr = ["1", "2", "3"];
$q = 0;
for ($t = 0; $t < 200; $t++) $q = $st($astr, 3);
echo "st=$q " . gettype($q) . "\n";

// 3) 混合元素（2.5 非 INT）→ guard fail → double
$fm = function ($a, $n) {
    $s = 0;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$am = [1, 2.5, 3];
$p = 0;
for ($t = 0; $t < 200; $t++) $p = $fm($am, 3);
echo "mx=$p " . gettype($p) . "\n";

// 4) acc 入口非 INT（$s=0.5 参数）→ guard fail → double
$fs = function ($a, $n, $st2) {
    $s = $st2;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$aa = [1, 2, 3];
$u = 0;
for ($t = 0; $t < 200; $t++) $u = $fs($aa, 3, 0.5);
echo "fs=$u " . gettype($u) . "\n";

// 5) 非 packed（unset 洞）→ guard fail → 解释器读存在的键
$fp = function ($a) {
    $s = 0;
    $i = 1;
    while ($i < 2) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$ap = [1, 2];
unset($ap[0]);   // 有洞 → packed=false；$a[1]=2 仍存在（零越界/警告）
$w = 0;
for ($t = 0; $t < 200; $t++) $w = $fp($ap);
echo "np=$w " . gettype($w) . "\n";

// 6) int64 溢出安全链：guard pass + 特化体内溢出 → RETURN(0) → 重跑 → double
$fo = function () {
    $s = 9223372036854775806;
    $a = [1, 1, 1];
    $i = 0;
    while ($i < 3) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$o = 0;
for ($t = 0; $t < 200; $t++) $o = $fo();
echo "of=$o " . gettype($o) . "\n";

// 7) DEC 倒序（idx=$i=DEC a1，形态守卫要求 INC/DEC 恒等 key 槽）
$fd = function ($a, $n) {
    $s = 0;
    $i = $n - 1;
    while ($i >= 0) {
        $s += $a[$i];
        $i--;
    }
    return $s;
};
$y = 0;
for ($t = 0; $t < 200; $t++) $y = $fd($ai, 10);
echo "dec=$y " . gettype($y) . "\n";

// 8) 空数组（n=0：全INT 扫描循环体不触 data；结果 0，两向皆稳）
$z = 0;
for ($t = 0; $t < 200; $t++) $z = $fi([], 0);
echo "emp=$z " . gettype($z) . "\n";

// 9) 编译后类型轮转（guard pass/fail 交替；末次 = int 数组）
$fr = function ($a, $n) {
    $s = 0;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$rt = 0;
for ($t = 0; $t < 300; $t++) {
    $rt = ($t % 2) ? $fr([1, 2, 3], 3) : $fr([1.5, 2.5], 2);
}
echo "rt=$rt " . gettype($rt) . "\n";

echo "END\n";
