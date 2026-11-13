<?php
/* 批4b-ii 链式 fwd 语义对拍（TPHP_CHAIN_FWD=1/0 三模输出须逐字一致）：
 * 覆盖：cv8 主路径（8 CV 键）/ N=2·N=3 短链 / 全 CONST 键链 / CV+CONST
 * 混链 / 动态 STR 键（hash==0 惰性）/ 混合值 map guard-fail / packed 基
 * guard-fail / absent 键 raise+catch 复跑 / 链中 INT 溢出 deopt 重跑 /
 * INT 键 map（non-STR 分派 helper）/ 身份撞车负例（键=acc，scan 拒）/
 * 键 CV 复用形 / guard 成败轮转 */
// EXPECT: c1=36000
// EXPECT: c2=1500
// EXPECT: c2b=2400
// EXPECT: c3=3000
// EXPECT: c3m=15000
// EXPECT: c3d=15000
// EXPECT: c4=1400
// EXPECT: c5=3000
// EXPECT: c6=E y=15
// EXPECT: c7=double 1.844674407371E+19
// EXPECT: c8=12000
// EXPECT: c9=8 2000
// EXPECT: c10=5200
// EXPECT: ok

/* ① cv8 主路径：8 CV 键链加（F F A F A ... A A(rmw) I 严格形态） */
function ch_cv8($n) {
    $m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
          "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
    $ka = "alpha"; $kb = "beta"; $kc = "gamma"; $kd = "delta";
    $ke = "epsilon"; $kf = "zeta"; $kg = "eta"; $kh = "theta";
    $sum = 0;
    for ($r = 0; $r < $n; $r++) {
        $sum += $m[$ka] + $m[$kb] + $m[$kc] + $m[$kd]
              + $m[$ke] + $m[$kf] + $m[$kg] + $m[$kh];
    }
    return $sum;
}
echo "c1=", ch_cv8(1000), "\n";               /* 36*1000 */

/* ② N=2 / N=3 短链 */
function ch_n2($n) {
    $m = ["x" => 1, "y" => 2];
    $kx = "x"; $ky = "y";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$kx] + $m[$ky];
    return $s;
}
function ch_n3($n) {
    $m = ["a" => 1, "b" => 2, "c" => 3];
    $ka = "a"; $kb = "b"; $kc = "c";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$ka] + $m[$kb] + $m[$kc];
    return $s;
}
echo "c2=", ch_n2(500), "\n";                 /* 3*500 */
echo "c2b=", ch_n3(400), "\n";                /* 6*400 */

/* ③ 全 CONST 键链 + CV/CONST 混链 */
function ch_const($n) {
    $m = ["a" => 1, "b" => 2, "c" => 3, "d" => 4];
    $s = 0;
    for ($r = 0; $r < $n; $r++) {
        $s += $m["a"] + $m["b"] + $m["c"] + $m["d"];
    }
    return $s;
}
function ch_mixk($n) {
    $m = ["a" => 10, "b" => 20, "c" => 30];
    $k = "b";
    $s = 0;
    for ($r = 0; $r < $n; $r++) {
        $s += $m["a"] + $m[$k] + $m["c"];
    }
    return $s;
}
echo "c3=", ch_const(300), "\n";              /* 10*300 */
echo "c3m=", ch_mixk(250), "\n";              /* 60*250 */

/* ④ 动态 STR 键（拼接 → hash==0 惰性路径） */
function ch_dyn($n) {
    $m = ["aa" => 4, "bb" => 5, "cc" => 6];
    $k1 = "a" . "a";
    $k2 = "b" . "b";
    $k3 = "c" . "c";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k1] + $m[$k2] + $m[$k3];
    return $s;
}
echo "c3d=", ch_dyn(1000), "\n";              /* (4+5+6)*1000 = 15000 */

/* ⑤ 混合值 map guard-fail（guard_ht 拒 → 解释器整函数接管，语义逐字同） */
function ch_guard($n) {
    $m = ["a" => 1.5, "b" => 2];
    $ka = "a"; $kb = "b";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$ka] + $m[$kb];
    return $s;
}
echo "c4=", ch_guard(400), "\n";              /* 3.5*400 = 1400 */

/* ⑥ packed 基链式（int 键字面量 → guard_ht 拒 → 解释器） */
function ch_packed($n) {
    $a = [1, 2, 3];
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $a[0] + $a[1] + $a[2];
    return $s;
}
echo "c5=", ch_packed(500), "\n";             /* 6*500 */

/* ⑦ absent 键 raise + catch + 复跑（F1 phi 已挂起时 F2 物化/复跑干净） */
function ch_absent($n) {
    $m = ["a" => 1, "b" => 2];
    $ka = "a"; $kb = "zz";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$ka] + $m[$kb];
    return $s;
}
$caught = [];
$y = 0;
for ($j = 0; $j < 120; $j++) {
    $caught = [];
    try {
        ch_absent(3);
    } catch (Error $e) {
        $caught[] = "E";
    }
    $y = ch_n2(5);
}
echo "c6=", implode("", $caught), " y=", $y, "\n";

/* ⑧ 链中 INT 溢出 → 快线 RETURN deopt → 解释器重跑得 double */
function ch_ovf($n) {
    $m = ["a" => 9223372036854775807, "b" => 1];
    $ka = "a"; $kb = "b";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$ka] + $m[$kb];
    return $s;
}
$o = ch_ovf(2);
echo "c7=", gettype($o), " ", $o, "\n";

/* ⑨ INT 键 map（non-STR 分派 → dimic_miss helper 线，phi 物化汇流） */
function ch_intkey($n) {
    $m = [1 => 10, 2 => 20];
    $k1 = 1; $k2 = 2;
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$k1] + $m[$k2];
    return $s;
}
echo "c8=", ch_intkey(400), "\n";             /* 30*400 */

/* ⑩ 身份撞车负例（键 CV==acc ⇒ scan identity 拒、照常正确）+
 *    键 CV 复用形（同键两 fetch ⇒ scan 键集去重放行） */
function ch_alias($n) {
    $m = [0 => 1, 2 => 3, 6 => 5];
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$s] + $m[$s];
    return $s;
}
function ch_reuse($n) {
    $m = ["a" => 4];
    $ka = "a";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$ka] + $m[$ka];
    return $s;
}
echo "c9=", ch_alias(2), " ", ch_reuse(250), "\n";   /* m[0]=1→2→m[2]*2=6 ; 8*250 */

/* ⑪ guard 成败轮转（全值 INT ↔ 混合值 map 交替重入） */
function ch_rot($m, $n) {
    $ka = "p"; $kq = "q";
    $s = 0;
    for ($r = 0; $r < $n; $r++) $s += $m[$ka] + $m[$kq];
    return $s;
}
$iv = ["p" => 1, "q" => 2];
$fv = ["p" => 1.5, "q" => 2];
$gi = 0.0;
$a = 0; $b = 0.0;
for ($j = 0; $j < 200; $j++) {
    $a = ch_rot($iv, 4);
    $b = ch_rot($fv, 4);
    $gi += $a + $b;
}
echo "c10=", $gi, "\n";                       /* 12+14=26 * 200 = 5200 */
echo "ok\n";
