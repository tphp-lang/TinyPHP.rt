<?php
// EXPECT: c1=225000
// EXPECT: c2=104995
// EXPECT: c3=225000000
// EXPECT: c4=435000
// EXPECT: c5=225000
// EXPECT: c6a=75000
// EXPECT: c6b=1485000
// EXPECT: c6c=caught
// A7-1 (PLAN/osr.md §8): FE_FETCH admission for OSR chunks -- foreach loops
// compile as chunks (header = the FE_FETCH pc; the trailing back-edge JMP
// triggers per-site counting/compilation). TPHP_JIT=0/1 must produce
// identical output and match PHP. Main stays try-free (n_ex>0 would refuse
// OSR for the whole function); main has exactly 16 distinct back-edge
// headers -- jit_cfg_ok (whole-function silent gate) refuses ALL OSR for a
// fn with >16, and the first draft's 17th header (c6c fill) killed every
// chunk (bisect: prefixes b12..b6ab all compiled, full did not).
//
// Shapes that turn out OUT of scope for chunk compilation (kept out here,
// recorded in osr.md s8 as known whole-JIT limits, not OSR gaps):
//   - break inside a loop body: the loop-cond close demands a back-edge
//     tail (emit_seq trap "body must back-edge or fail", jit.c L5828) --
//     function-level JIT refuses such loops too (verified with a minimal
//     for+break function: FAILED emit trap=3), so a break face can only
//     run interpreted; c3 is continue-only.
//   - nested-in-arg calls (osr6_fn(intval($v)) form): DAO 5C gate ① is
//     function-level only ("OSR chunk resume pc does not cover inner
//     sequences"), OSR INIT_FCALL with an active fold state fails at
//     jit.c L5688; c4 uses standalone sequential calls instead.
// c1 packed foreach (val only), int body
$arr1 = [];
for ($i = 0; $i < 30000; $i++) { $arr1[] = $i & 15; }
$s1 = 0;
foreach ($arr1 as $v) { $s1 += $v; }
echo "c1=$s1\n";

// c2 hash foreach key=>val: exercises both COPY bindings (val + key)
$m2 = [];
for ($i = 0; $i < 30000; $i++) { $m2["k$i"] = $i & 7; }
$s2 = 0;
foreach ($m2 as $k => $v) {
    $s2 += ($k === "k7") ? 2 : $v;
}
echo "c2=$s2\n";

// c3 continue-only multi-site (the A3 hang-accounted shape): the continue
// JMP is a second back-edge site -- its own per-site chunk; the trailing
// site's chunk contains the mid-region continue JMP, closed via the
// batchC slice-1 MERGE_N reconcile. 15000 continues >= the 10000 OSR
// threshold so BOTH sites attempt compilation.
$arr3 = [];
for ($i = 0; $i < 30000; $i++) { $arr3[] = $i; }
$s3 = 0;
foreach ($arr3 as $v) {
    if (($v & 1) === 0) continue;
    $s3 += $v;
}
echo "c3=$s3\n";

// c4 builtin + user call in the body as a STANDALONE sequence (batch 2-2
// OSR call replay); the nested-in-arg form is refused (DAO 5C, see header)
function osr6_fn($x) { return $x + 1; }
$arr4 = [];
for ($i = 0; $i < 30000; $i++) { $arr4[] = "1" . ($i & 7); }
$s4 = 0;
foreach ($arr4 as $v) {
    $x4 = intval($v);
    $s4 += osr6_fn($x4);
}
echo "c4=$s4\n";

// c5 foreach over a Generator: FE_FETCH takes the generator branch inside
// the chunk (pad black-box; suspend/resume + exhaustion must stay correct).
// NOTE: re-foreach over an exhausted generator raises in tphp (divergence
// noted), so exhaustion is covered by full consumption only.
function osr6_gen($n) { for ($i = 0; $i < $n; $i++) { yield $i & 15; } }
$s5 = 0;
foreach (osr6_gen(30000) as $v) { $s5 += $v; }
echo "c5=$s5\n";

// c6a foreach inside while: the while chunk's region contains the
// foreach's FE_RESET -> attempted (15000 while back-edges) and rejected
// by osr_body_ok (batch-5 nested loop x FE guard); the foreach site still
// compiles as its own chunk; outer loop stays interpreter -- values identical
$arr6a = [5];
$w6 = 0; $t6 = 0;
while ($t6 < 15000) {
    $t6++;
    foreach ($arr6a as $v) { $w6 += $v; }
}
echo "c6a=$w6\n";

// c6b foreach inside foreach: the outer site's back-edge counter only
// reaches 300 (< threshold) so it never attempts -- outer stays
// interpreter; the inner site accumulates 30000 back-edges and compiles
$inner6 = []; for ($i = 0; $i < 100; $i++) { $inner6[] = $i; }
$outer6 = []; for ($i = 0; $i < 300; $i++) { $outer6[] = $i; }
$s6b = 0;
foreach ($outer6 as $a) {
    foreach ($inner6 as $b) { $s6b += $b; }
}
echo "c6b=$s6b\n";

// c6c raise face: bcall (count) raises TypeError on a scalar element
// mid-foreach inside a try-free loop function -> exception unwinds through
// the chunk (fail = exception, no deopt re-run -> no double cursor advance);
// runner catches outside so the loop function's n_ex stays 0.
// Filler statements push n_ops past the 96 eager gate: an eligible
// <=96-op function is function-level compiled on first call and its loop
// never counts back-edges (first draft's 14-op osr6_c6loop proved this --
// zero OSR lines for the site), so the raise face would never reach a chunk
function osr6_c6loop($arr) {
    $s = 0; $p = 0;
    $p = $p + 1; $p = $p + 2; $p = $p + 3; $p = $p + 4; $p = $p + 5;
    $p = $p + 6; $p = $p + 7; $p = $p + 8; $p = $p + 9; $p = $p + 10;
    $p = $p + 11; $p = $p + 12; $p = $p + 13; $p = $p + 14; $p = $p + 15;
    $p = $p + 16; $p = $p + 17; $p = $p + 18; $p = $p + 19; $p = $p + 20;
    $p = $p + 21; $p = $p + 22; $p = $p + 23; $p = $p + 24; $p = $p + 25;
    $p = $p + 26; $p = $p + 27; $p = $p + 28; $p = $p + 29; $p = $p + 30;
    $p = $p + 31; $p = $p + 32; $p = $p + 33; $p = $p + 34; $p = $p + 35;
    $p = $p + 36; $p = $p + 37; $p = $p + 38; $p = $p + 39; $p = $p + 40;
    $p = $p + 41; $p = $p + 42;   /* exactly 96 ops == the eager gate; 2 more
                                     push it over (n_ops must be > 96) */
    foreach ($arr as $v) { $s += count($v); }
    return $s + $p;
}
function osr6_c6run($arr) {
    try {
        return "ok=" . osr6_c6loop($arr);
    } catch (Throwable $e) {
        return "caught";
    }
}
$mix6 = [];
for ($i = 0; $i < 30000; $i++) { $mix6[] = ($i === 25000) ? 42 : [$i]; }
echo "c6c=" . osr6_c6run($mix6) . "\n";
