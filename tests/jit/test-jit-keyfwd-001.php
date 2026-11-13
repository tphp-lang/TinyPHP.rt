<?php
/* 批4b-i key-fwd 语义对拍（TPHP_KEY_FWD=1/0 三模输出须逐字一致）：
 * 覆盖：命中热路径 / 键不在表（dimic_miss 物化）/ 混合类型键（non-STR
 * 分派物化）/ 动态 STR 键 hash==0 惰性路径（1684 bug 修正回归面）/
 * 物化后 deopt catch 复跑（槽状态干净）/ STR 键 rc 稳定（多轮不涨不漏）
 * / guard-fail 混合值轮转 */
// EXPECT: hit=15
// EXPECT: oob=E y=15
// EXPECT: mixed=600
// EXPECT: dyn=36
// EXPECT: rc=3
// EXPECT: rot=3 3.5 1300
// EXPECT: ok

function kfwd_hit($v, $k, $n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) {
        $s += $v[$k[$i]];
    }
    return $s;
}
$keys = ["a", "b", "c", "d"];
$vals = ["a" => 1, "b" => 2, "c" => 4, "d" => 8];
$s = 0;
for ($j = 0; $j < 300; $j++) {
    $s = kfwd_hit($vals, $keys, 4);
}
echo "hit=", $s, "\n";                       /* 15 */

/* 键不在表 → F2 dimic_miss 物化 + helper raise → deopt 解释器复跑 */
$partial = ["a" => 10, "c" => 30];
$ek = ["a", "b", "c", "d"];
$caught = [];
for ($j = 0; $j < 120; $j++) {
    $caught = [];
    try {
        $x = kfwd_hit($partial, $ek, 4);
    } catch (Error $e) {
        $caught[] = "E";
    }
    /* 物化槽状态干净：catch 后继续跑正常形态不炸 */
    $y = kfwd_hit($vals, $keys, 4);
}
echo "oob=", implode("", $caught), " y=", $y, "\n";  /* E y=15 */

/* 混合类型键（INT 键走 non-STR 分派线 → 物化 + C norm） */
$mixed = [1 => 100, 2 => 200];
$mk = [1, 2, 1, 2];
$m = 0;
for ($j = 0; $j < 120; $j++) {
    $m = kfwd_hit($mixed, $mk, 4);
}
echo "mixed=", $m, "\n";                     /* 600 */

/* 动态 STR 键（tstr_new → hash==0 惰性路径，1684 修正回归面） */
$dk = [];
$dv = [];
for ($i = 0; $i < 8; $i++) {
    $dk[$i] = "k" . ($i + 1);               /* 拼接产生非字面量串 */
    $dv["k" . ($i + 1)] = $i + 1;
}
$d = 0;
for ($j = 0; $j < 200; $j++) {
    $d = kfwd_hit($dv, $dk, 8);
}
echo "dyn=", $d, "\n";                       /* 36 */

/* STR 键 rc 稳定：多轮命中+物化轮转，键串不因 kfwd 涨/漏 */
$rk = ["x", "y"];
$rv = ["x" => 1, "y" => 2];
for ($j = 0; $j < 500; $j++) {
    $r1 = kfwd_hit($rv, $rk, 2);
    if (($j % 50) == 49) {
        try { kfwd_hit($rv, ["x", "z"], 2); } catch (Error $e) {}
    }
}
echo "rc=", $r1, "\n";                       /* 3 */

/* guard-fail 轮转：ht（全值 INT）↔ 混合值（guard 拒）交替 */
$iv = ["p" => 1, "q" => 2];
$fv = ["p" => 1.5, "q" => 2];
$gk = ["p", "q"];
$gi = 0.0;
for ($j = 0; $j < 200; $j++) {
    $a = kfwd_hit($iv, $gk, 2);
    $b = kfwd_hit($fv, $gk, 2);
    $gi += $a + $b;
}
echo "rot=", $a, " ", $b, " ", $gi, "\n";    /* 3 3.5 1300 */
echo "ok\n";