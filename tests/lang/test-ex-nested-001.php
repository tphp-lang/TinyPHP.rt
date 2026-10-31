<?php
// Exception handler resolution parity (P0-17 throw-profile session, 10-31):
// (1) same-frame nested try — inner catch type mismatch must fall through
//     to the OUTER same-frame try (was: innermost region miss popped the
//     frame => Uncaught; PHP catches outer). Regression for the unified
//     frame_find_handler (unwind + throw fast path share one resolver).
// (2) cross-frame variant, (3) direct inner match, (4) throw in loop body.
// TPHP_JIT=0/1 must produce identical output; EXPECT = PHP 8.5 verbatim.

try {
    try { throw new Exception("a"); } catch (RuntimeException $e) { echo "inner\n"; }
} catch (Exception $e) { echo "outer:" . $e->getMessage() . "\n"; }

function xf() { throw new Exception("b"); }
function xg() {
    try { xf(); } catch (RuntimeException $e) { echo "g-inner\n"; }
}
try { xg(); } catch (Exception $e) { echo "g-outer:" . $e->getMessage() . "\n"; }

try {
    try { throw new Exception("c"); } catch (Exception $e) { echo "inner2:" . $e->getMessage() . "\n"; }
} catch (Exception $e) { echo "outer2\n"; }

for ($i = 0; $i < 3; $i++) {
    try { if ($i) echo " "; if ($i == 1) throw new Exception("d$i"); echo "L$i"; } catch (Exception $e) { echo "C" . $e->getMessage(); }
}
echo "\n";

// three-deep nesting: innermost mismatch, middle mismatch, outer matches
try {
    try {
        try { throw new Exception("e"); } catch (LogicException $e) { echo "n3\n"; }
    } catch (RuntimeException $e) { echo "n2\n"; }
} catch (Exception $e) { echo "n1:" . $e->getMessage() . "\n"; }

// same-frame inner match takes priority over outer (innermost first)
try {
    try { throw new Exception("f"); } catch (Exception $e) { echo "m-inner:" . $e->getMessage() . "\n"; }
} catch (Exception $e) { echo "m-outer\n"; }
// EXPECT: outer:a
// EXPECT: g-outer:b
// EXPECT: inner2:c
// EXPECT: L0 Cd1 L2
// EXPECT: n1:e
// EXPECT: m-inner:f
