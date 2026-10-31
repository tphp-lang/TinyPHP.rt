<?php
// P0-17③ batch C slice-1: acc pay loop-phi + pay-only store (fold fast line).
// Regression net: (1) fold dst slot used operand-kind as index (OPD_CV==2
// collided with the bench acc slot; any other slot corrupted a stranger CV)
// (2) phi soundness across slow/float/overflow/string paths, sibling loops,
// post-loop slot currency. TPHP_JIT=0/1 must produce identical output.
// Loops exceed OSR back-edge threshold (10000) so top-level loops compile.

// 1) acc at CV slot 0 (declared first) -- tr/ar regression shape.
$s0 = 0;
$a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 10; $i++) $s0 += $a[$i];
}
echo "s0=$s0\n";

// 2) acc at CV slot 3+ with a preceding padding local.
$s3 = 0;
$b = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
$pad = 7;
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 10; $i++) $s3 += $b[$i];
}
echo "s3=$s3 pad=$pad\n";

// 3) float element mid-array: fast-line INT check fails -> slow line.
$cf = 0;
$af = [1, 2, 3.5, 4, 5];
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 5; $i++) $cf += $af[$i];
}
echo "cf=$cf " . gettype($cf) . "\n";

// 4) float acc from the start: entry tag non-INT -> every iteration slow.
$fa = 0.5;
$ai = [1, 2, 3];
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 3; $i++) $fa += $ai[$i];
}
echo "fa=$fa " . gettype($fa) . "\n";

// 5) int64 overflow: speculative add must bail to slow (float promotion).
$of = 9223372036854775806;
$one = [1, 1, 1];
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 3; $i++) $of += $one[$i];
}
echo "of=$of " . gettype($of) . "\n";

// 6) numeric-string acc: entry STR -> all slow, result int.
$ss = "5";
$as = [1, 2, 3];
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 3; $i++) $ss += $as[$i];
}
echo "ss=$ss " . gettype($ss) . "\n";

// 7) while-form of the same RMW shape.
$ws = 0;
$aw = [1, 2, 3, 4];
$n = 20000;
for ($r = 0; $r < $n; $r++) {
    $k = 0;
    while ($k < 4) {
        $ws += $aw[$k];
        $k++;
    }
}
echo "ws=$ws\n";

// 8) sibling loops reusing one acc: fresh phi per loop, slot stays current.
$sb = 0;
$ab = [1, 2, 3, 4, 5];
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 5; $i++) $sb += $ab[$i];
    for ($i = 0; $i < 5; $i++) $sb += $ab[$i];
}
echo "sb=$sb\n";

// 9) non-fold interference: extra statement in body -> guard bails,
//    B' full-store fallback must stay correct.
$si = 0;
$xi = 0;
$ai2 = [1, 2, 3, 4, 5];
for ($r = 0; $r < 20000; $r++) {
    for ($i = 0; $i < 5; $i++) {
        $si += $ai2[$i];
        $xi = $si;
    }
}
echo "si=$si xi=$xi\n";
// EXPECT: s0=1100000
// EXPECT: s3=1100000 pad=7
// EXPECT: cf=310000 double
// EXPECT: fa=120000.5 double
// EXPECT: of=9.2233720368548E+18 double
// EXPECT: ss=120005 integer
// EXPECT: ws=200000
// EXPECT: sb=600000
// EXPECT: si=300000 xi=300000
