<?php
/* 批4c 单 fetch ht 化语义对拍（TPHP_ONEKEY=1/0 三模输出须逐字一致）：
 * 覆盖：1key 主路径（CV STR 键）/ const STR lit 单查 / 动态 STR 键
 * （hash==0 惰性）/ CV INT 键 guard-fail 降级 / packed 基 INT 键降级 /
 * 混合值 guard-fail / absent raise+catch 复跑 / 溢出 deopt double /
 * read 形态对照（键==idx 分流不受影响）/ 键==acc 身份撞车拒 /
 * int-lit 键 scan 拒 / 多轮稳定 */
// EXPECT: ok1=600000
// EXPECT: ok2=200000
// EXPECT: dyn=400000
// EXPECT: ikey=4000
// EXPECT: pk=3000
// EXPECT: mixed=1500
// EXPECT: abs=E y=15
// EXPECT: ovf=double 1.844674407371E+19
// EXPECT: read=999000
// EXPECT: alias=12
// EXPECT: intlit=2000
// EXPECT: rot=3 3
// EXPECT: rc=1200
// EXPECT: ok

/* ① 1key 主路径：CV STR 键 hash 单查 */
function ok_cv($n) {
    $m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
          "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
    $k = "gamma";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k];
    return $s;
}
echo "ok1=", ok_cv(200000), "\n";        /* 3*200000 */

/* ② const STR lit 单查 */
function ok_lit($n) {
    $m = ["alpha" => 1, "beta" => 2, "gamma" => 3];
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m["alpha"];
    return $s;
}
echo "ok2=", ok_lit(200000), "\n";        /* 1*200000 */

/* ③ 动态 STR 键（拼接 → hash==0 惰性路径） */
function ok_dyn($n) {
    $m = ["kk1" => 4, "kk2" => 6];
    $k = "k" . "k1";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k];
    return $s;
}
echo "dyn=", ok_dyn(100000), "\n";        /* 4*100000 */

/* ④ CV INT 键 → guard ktag==STR fail → 解释器接管（语义逐字同） */
function ok_ikey($n) {
    $m = [1 => 10, 2 => 20];
    $k = 1;
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k];
    return $s;
}
echo "ikey=", ok_ikey(400), "\n";         /* 10*400 */

/* ⑤ packed 基 INT 键 CV → 同上降级 */
function ok_pk($n) {
    $a = [10, 20, 30];
    $k = 0;
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $a[$k];
    return $s;
}
echo "pk=", ok_pk(300), "\n";             /* 10*300 */

/* ⑥ 混合值 map（STR 键过、guard_ht 值证 fail）→ 降级 */
function ok_mixed($n) {
    $m = ["a" => 1.5, "b" => 2];
    $k = "a";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k];
    return $s;
}
echo "mixed=", ok_mixed(1000), "\n";      /* 1.5*1000 */

/* ⑦ absent 键 raise + catch + 复跑（物化/槽状态干净） */
function ok_abs($n) {
    $m = ["a" => 1, "b" => 2];
    $k = "zz";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k];
    return $s;
}
$caught = [];
$y = 0;
for ($j = 0; $j < 120; $j++) {
    $caught = [];
    try {
        ok_abs(3);
    } catch (Error $e) {
        $caught[] = "E";
    }
    $k2 = "gamma";
    $m2 = ["gamma" => 15];
    $y = 0;
    for ($r = 0; $r < 1; $r++) $y += $m2[$k2];
}
echo "abs=", implode("", $caught), " y=", $y, "\n";

/* ⑧ 链外单查 INT 溢出 → 快线 RETURN deopt → 解释器重跑得 double */
function ok_ovf($n) {
    $m = ["a" => 9223372036854775807];
    $k = "a";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k];
    return $s;
}
$o = ok_ovf(2);
echo "ovf=", gettype($o), " ", $o, "\n";

/* ⑨ read 形态对照：键==idx 走 packed 路径（分流零误伤） */
function ok_read($n) {
    $a = [];
    for ($i = 0; $i < 1000; $i++) $a[] = $i;
    $s = 0;
    for ($r = 0; $r < $n; $r++) {
        for ($i = 0; $i < 1000; $i++) $s += $a[$i];
    }
    return $s;
}
echo "read=", ok_read(2), "\n";           /* 499500*2*2 */

/* ⑩ 键==acc 身份撞车（scan identity 拒 → 通用 JIT 照常正确） */
function ok_alias($n) {
    $m = [0 => 5, 5 => 7];
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$s];
    return $s;
}
echo "alias=", ok_alias(2), "\n";         /* 0+5=5（第一次 m[0]），5+7=12 */

/* ⑪ int 字面量键（非 STR lit → scan 拒 → 通用 JIT 照常正确） */
function ok_intlit($n) {
    $m = [7 => 5, 0 => 1];
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[7];
    return $s;
}
echo "intlit=", ok_intlit(400), "\n";     /* 5*400 */

/* ⑫ guard 成败轮转（STR 键全值 INT ↔ INT 键 / 混合值交替重入） */
function ok_rot($m, $k, $n) {
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k];
    return $s;
}
$iv = ["p" => 1, "q" => 2];
$fv = ["p" => 1.5, "q" => 2];
$ia = [7 => 1, 8 => 2];
$a = 0; $b = 0.0;
for ($j = 0; $j < 200; $j++) {
    $a = ok_rot($iv, "p", 3);
    $b = ok_rot($fv, "p", 2);
    $c = ok_rot($ia, 7, 2);   /* INT 键轮转（guard 降级路径轮换） */
}
echo "rot=", $a, " ", $b, "\n";           /* 3 3 */

/* ⑬ 多轮稳定（键槽/物化槽 rc 与状态） */
$last = 0;
for ($j = 0; $j < 500; $j++) {
    $last = ok_cv(400);
}
echo "rc=", $last, "\n";                  /* 3*400*500 累不进——每次重算 1200？*/
echo "ok\n";
