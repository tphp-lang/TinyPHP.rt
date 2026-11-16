<?php
// B1 break-in-loop admission (PLAN/osr.md s9): loops containing
// `if (cond) break;` previously fell to the interpreter 100% of the time
// (loopcond "body must back-edge or fail"). B1 adds loop_exit[] + a bare
// break pre-scan in the if handler (find_join misparses a break forward
// jump as an else jend) + bp_pend stash + exit MERGE_N; the conservative
// boundaries stay rejected (multi-stmt then, break N, switch break, >2
// escapes per layer, ||-in-loop). TPHP_JIT=0/1 must produce identical
// output and match PHP 8.5.1.
// EXPECT: c1=15
// EXPECT: c2=15
// EXPECT: c3=0
// EXPECT: c4=15
// EXPECT: c5=29
// EXPECT: c6=2
// EXPECT: c7=1770
// EXPECT: c8=10
// EXPECT: c9=15
// EXPECT: c10=1250025256

// c1 for + if-break with a call in the cond (canonical smoke shape; eager
// function-level compile -- verify with TPHP_JIT_DEBUG=1 "compile(c1_)")
function c1_for_break($a, $lim) { $s = 0;
    for ($i = 0; $i < count($a); $i++) { if ($a[$i] > $lim) break; $s += $a[$i]; }
    return $s; }

// c2 while + body-top if break
function c2_while_break($a, $lim) { $s = 0; $i = 0;
    while ($i < count($a)) { if ($a[$i] > $lim) break; $s += $a[$i]; $i++; }
    return $s; }

// c3 if/else both branches break (three-side shape): stays conservatively
// rejected under B1 v1 (both-side escape) -- interpreter parity only
function c3_both_break($a) { $s = 0;
    for ($i = 0; $i < count($a); $i++) {
        if ($a[$i] > 3) { break; } else { break; }
        $s += 1;
    }
    return $s; }

// c4 continue + break in separate ifs (lb and bp pendings coexist)
function c4_continue_break($a, $lim) { $s = 0;
    for ($i = 0; $i < count($a); $i++) {
        if ($a[$i] == 0) continue;
        if ($a[$i] > $lim) break;
        $s += $a[$i];
    }
    return $s; }

// c5 nested loops: inner break only (outer loop unaffected)
function c5_nested_break($a, $lim) { $s = 0;
    for ($i = 0; $i < count($a); $i++) {
        for ($j = 0; $j < 3; $j++) { if ($j == 2) break; $s += 1; }
        if ($a[$i] > $lim) break;
        $s += $a[$i];
    }
    return $s; }

// c6 break 2: cross-loop escape stays rejected (value parity via interpreter)
function c6_break2($a) { $s = 0;
    for ($i = 0; $i < count($a); $i++) {
        for ($j = 0; $j < 3; $j++) { if ($j == 2) break 2; $s += 1; }
        $s += $a[$i];
    }
    return $s; }

// c7 kv fast-line + break + accumulator read after the loop
function c7_kv_break($m, $keys) { $s = 0;
    for ($k = 0; $k < count($keys); $k++) {
        $key = $keys[$k];
        if (!isset($m[$key])) break;
        $s += $m[$key];
    }
    return $s; }

// c8 switch + break inside a loop: pre-B1 behavior locked (switch owns the
// break target -- stays rejected, no regression)
function c8_switch_break($a, $lim) { $s = 0;
    for ($i = 0; $i < count($a); $i++) {
        switch ($a[$i] % 2) { case 0: $s += 1; break; case 1: $s += 2; break; }
        if ($a[$i] > $lim) break;
    }
    return $s; }

// c9 foreach + if-break (B1-2: same loopcond exit point, zero new code)
function c9_foreach_break($a, $lim) { $s = 0;
    foreach ($a as $v) { if ($v > $lim) break; $s += $v; }
    return $s; }

$a = [];
for ($i = 0; $i < 64; $i++) { $a[] = $i % 10; }
$m = [];
for ($i = 0; $i < 100; $i++) { $m["k$i"] = $i; }
$keys = [];
for ($i = 0; $i < 60; $i++) { $keys[] = "k$i"; }

for ($r = 0; $r < 300; $r++) {
    $v1 = c1_for_break($a, 5);
    $v2 = c2_while_break($a, 5);
    $v3 = c3_both_break($a);
    $v4 = c4_continue_break($a, 5);
    $v5 = c5_nested_break($a, 5);
    $v6 = c6_break2($a);
    $v7 = c7_kv_break($m, $keys);
    $v8 = c8_switch_break($a, 5);
    $v9 = c9_foreach_break($a, 5);
}
echo "c1=$v1\n";
echo "c2=$v2\n";
echo "c3=$v3\n";
echo "c4=$v4\n";
echo "c5=$v5\n";
echo "c6=$v6\n";
echo "c7=$v7\n";
echo "c8=$v8\n";
echo "c9=$v9\n";

// c10 OSR-face bare break: OP_POW after the loop kills eager eligibility
// (interpreter counts back-edges), the chunk compiles at the 10000 back-edge
// threshold, then the break fires inside JIT code at i>50000.
function c10_osr_break($lim) { $s = 0;
    for ($i = 0; $i < $lim; $i++) { if ($i > 50000) break; $s += $i; }
    $e = 2 ** 8;
    return $s + $e;
}
echo "c10=", c10_osr_break(60000), "\n";
