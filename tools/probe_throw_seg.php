<?php
// P0-17 throw/catch component decomposition. Top-level loops only:
// try/catch in main => n_ex>0 => OSR refused (tphp) => pure interpreter.
// Segments isolate: loop floor / object create / create+throw+catch /
// cross-frame unwind / finally chain. Run plain (no opcache) on PHP side
// and TPHP_JIT=0 on tphp for interpreter-vs-interpreter, plus JIT-on
// reference columns.

class E0 {}

$n0 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) { $n0++; }
$t1 = microtime(true);
echo "SEG floor-loop      " . round(($t1 - $t0) * 1000, 1) . " ms\n";

$n1 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) { $e = new Exception("v"); if ($e === null) $n1++; }
$t1 = microtime(true);
echo "SEG create-ex       " . round(($t1 - $t0) * 1000, 1) . " ms\n";

$n2 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) { $o = new E0(); if ($o === null) $n2++; }
$t1 = microtime(true);
echo "SEG create-plain    " . round(($t1 - $t0) * 1000, 1) . " ms\n";

$n3 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) {
    try { throw new Exception("v"); } catch (Exception $e) { $n3++; }
}
$t1 = microtime(true);
echo "SEG throw-same      " . round(($t1 - $t0) * 1000, 1) . " ms\n";

function thrower($m) { throw new Exception($m); }

$n4 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) {
    try { thrower("v"); } catch (Exception $e) { $n4++; }
}
$t1 = microtime(true);
echo "SEG throw-cross     " . round(($t1 - $t0) * 1000, 1) . " ms\n";

$n5 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) {
    try { throw new Exception("v"); } catch (Exception $e) { $n5++; } finally { $n5++; }
}
$t1 = microtime(true);
echo "SEG throw-fin-catch " . round(($t1 - $t0) * 1000, 1) . " ms\n";

echo "check=$n0/$n1/$n2/$n3/$n4/$n5\n";
