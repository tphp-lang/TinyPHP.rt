<?php
// OSR batch-2-1 edge cases (PLAN/osr.md): FETCH_DIM_R raise/deopt behavior.
// This file's try/catch blocks set main's n_ex>0, so compile_osr refuses the
// whole function (documented conservative gate) -- everything stays on the
// interpreter. Value: proves the refusal path is safe and exception semantics
// match between TPHP_JIT=0/1. Raise-through-JIT edges with OSR-compiled loops
// are covered once batch 2 lifts the n_ex gate (osr.md).

// 1) undefined key inside a hot loop: helper raise must not cross IR;
//    interpreter raises per round and the catch keeps the loop alive.
//    keys cycle 0..4; key 1 -> "one", key 3 -> "three" ((int)"one"=0),
//    others raise (+100 each): 4000 cycles * (0 + 0 + 3*100) = 1200000.
$hits = 0;
$small = [1 => "one", 3 => "three"];
for ($i = 0; $i < 20000; $i++) {
    try {
        $hits += (int)$small[$i % 5];
    } catch (Throwable $e) {
        $hits += 100;
    }
}
echo "raise=$hits\n";

// 2) non-array base: TypeError raised per access, caught: 20000.
$bad = 42;
$cnt = 0;
for ($i = 0; $i < 20000; $i++) {
    try {
        $cnt += (int)$bad[$i % 3];
    } catch (Throwable $e) {
        $cnt += 1;
    }
}
echo "nonarr=$cnt\n";

// 3) packed overrun: int key past n on a packed array -> undefined key.
//    keys cycle 0..9 over n=5: keys 5..9 raise (+1), keys 0..4 add values:
//    2000 cycles * (0+1+2+3+4 + 5*1) = 2000 * 15 = 30000.
$ov = 0;
$p = [0, 1, 2, 3, 4];
for ($i = 0; $i < 20000; $i++) {
    try {
        $ov += $p[$i % 10];
    } catch (Throwable $e) {
        $ov += 1;
    }
}
echo "overrun=$ov\n";
// EXPECT: raise=1200000
// EXPECT: nonarr=20000
// EXPECT: overrun=30000
