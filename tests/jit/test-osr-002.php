<?php
// OSR batch-2-1 correctness (PLAN/osr.md): FETCH_DIM_R in hot loop bodies.
// Covers both IC fast paths (packed int direct / DimIC hash chain) plus
// tmp-slot key operands. Raise/deopt edges live in test-osr-003.php
// (try/catch sets main's n_ex>0 -> whole function refuses OSR compile).
// TPHP_JIT=0/1 must produce identical output.

// 1) kv-read bench shape: nested dim reads, string keys, 8-key rotation.
//    20000*8 back-edges >> threshold 10000 -> loops OSR-compile.
$m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
      "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
$keys = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
$sum = 0;
for ($r = 0; $r < 20000; $r++) {
    for ($k = 0; $k < 8; $k++) $sum += $m[$keys[$k]];
}
echo "kv=$sum\n";

// 2) packed int key: packed direct-read fast path.
$a = [];
for ($i = 0; $i < 1000; $i++) $a[] = $i;
$psum = 0;
for ($r = 0; $r < 1000; $r++) {
    for ($i = 0; $i < 1000; $i++) $psum += $a[$i];
}
echo "packed=$psum\n";

// 3) key from another dim read in the same loop (tmp-slot key operand).
$idx = [0, 2, 4, 6, 8];
$vals = [10, 11, 12, 13, 14, 15, 16, 17, 18, 19];
$vsum = 0;
for ($r = 0; $r < 20000; $r++) {
    for ($k = 0; $k < 5; $k++) $vsum += $vals[$idx[$k]];
}
echo "nested=$vsum\n";
// EXPECT: kv=720000
// EXPECT: packed=499500000
// EXPECT: nested=1400000
