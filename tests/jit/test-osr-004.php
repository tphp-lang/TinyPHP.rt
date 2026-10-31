<?php
// P1-12 batch 2-2: folded CALL sequences + property ops in OSR chunks.
// TPHP_JIT=0/1 must produce identical output (deopt-resume = sequence
// start for calls, cur_pc for prop pads, exception paths propagate).
// NOTE: main must stay try-free (n_ex>0 refuses OSR for the whole
// function, batch-1 gate) -- the raise/propagate case runs in its own
// try-free loop function, caught at top level.
// EXPECT: c1=450000
// EXPECT: c2=105000
// EXPECT: c3=105000:7
// EXPECT: c4=3464872
// EXPECT: c5=0:boom:1
// EXPECT: c6=900944616:29999

// 1) plain user call inside the loop (INIT_FCALL..DO_FCALL fold, static
//    direct call to an eligible callee)
function osr_double($x) { return $x * 2; }
$s1 = 0;
for ($i = 0; $i < 30000; $i++) {
    $s1 += osr_double($i & 15);
}
echo "c1=$s1\n";

// 2) builtin call in the loop (ic|1 -> tphp_jit_bcall, fail = exception)
$s2 = 0;
for ($i = 0; $i < 30000; $i++) {
    $s2 += strlen("ab") + ($i & 3);
}
echo "c2=$s2\n";

// 3) property read/write inside the loop (single-op replay pads)
class OsrBox {
    public int $n = 0;
    public int $acc = 0;
}
$b = new OsrBox();
for ($i = 0; $i < 30000; $i++) {
    $b->n = $i & 7;
    $b->acc += $b->n;
}
echo "c3={$b->acc}:{$b->n}\n";

// 4) dynamic call $f($x) inside the loop (INIT_DYN fold -> call_dyn
//    trampoline / opseq deep-run; callee is a captured closure)
function osr_mk($m) {
    return function($x) use ($m) { return $x + $m; };
}
$f = osr_mk(100);
$s4 = 0;
for ($i = 0; $i < 30000; $i++) {
    $s4 += $f($i & 31);
}
echo "c4=$s4\n";

// 5) callee raises mid-loop: the OSR chunk must let the exception
//    propagate exactly once (raises==1 proves no deopt-replay re-ran
//    the call); loop function stays try-free so the chunk still compiles
$raises = 0;
function osr_boom($k) {
    global $raises;
    if ($k == 5000) {
        $raises++;
        throw new RuntimeException("boom");
    }
    return $k;
}
function osr_loop5() {
    $s5 = 0;
    for ($i = 0; $i < 30000; $i++) {
        $s5 += osr_boom($i);
    }
    return $s5;
}
function osr_c5_runner() {
    try {
        return osr_loop5();
    } catch (RuntimeException $e) {
        return $e->getMessage();
    }
}
$s5 = osr_c5_runner();
$caught = "none";
if ($s5 === "boom") { $caught = "boom"; $s5 = 0; }
echo "c5=$s5:$caught:$raises\n";

// 6) expression args (computed before the fold, outside replay range):
//    deopt-replay from the sequence start must not re-run them
$s6 = 0;
function osr_add($a, $b2) { return $a + $b2; }
$g = 0;
for ($i = 0; $i < 30000; $i++) {
    $g = $i;
    $s6 += osr_add($g * 2 + 1, $i & 63);
}
echo "c6=$s6:$g\n";
