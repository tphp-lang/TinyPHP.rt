<?php
// OSR batch-1 correctness (PLAN/osr.md): interpreter hot loops enter the
// JIT via on-stack replacement; TPHP_JIT=0/1 must produce identical output.
// EXPECT: sum1=449985000
// EXPECT: sum2=449985000
// EXPECT: u=208341667
// EXPECT: v=25000
// EXPECT: w=30000
// EXPECT: nest=312487500

// 1) plain for-loop accumulate (back-edge via OP_JMP, threshold 10000)
$s = 0;
for ($i = 0; $i < 30000; $i++) {
    $s += $i;
}
echo "sum1=$s\n";

// 2) while-loop form
$j = 0;
$t = 0;
while ($j < 30000) {
    $t += $j;
    $j++;
}
echo "sum2=$t\n";

// 3) break + continue inside the hot loop
$u = 0;
for ($k = 0; $k < 30000; $k++) {
    if ($k % 3 === 0) continue;
    if ($k > 25000) break;
    $u += $k;
}
echo "u=$u\n";

// 4) INC in hot loop
$v = 0;
for ($m = 0; $m < 25000; $m++) {
    $v++;
}
echo "v=$v\n";

// 5) nested loops: inner back-edge hot (3000*9), outer below threshold
$w = 0;
for ($a = 0; $a < 3000; $a++) {
    for ($b = 0; $b < 10; $b++) {
        $w++;
    }
}
echo "w=$w\n";

// 6) while + conditional inside the loop
$n = 0;
$acc = 0;
while ($n < 25000) {
    $acc += $n;
    if ($acc > 999999999) {
        $acc = 0;
    }
    $n++;
}
echo "nest=$acc\n";
