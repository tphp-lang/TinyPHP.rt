<?php
// call-overhead decomposition: pure method call / new-only / noop method.
class C0 { }                      // no ctor, no dtor
class N0 { public function noop() {} }
$o0 = new N0();
$n1 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 1000000; $i++) { $o0->noop(); $n1++; }
$t1 = microtime(true);
echo "SEG pure-call       " . round(($t1 - $t0) * 1000, 1) . " ms\n";
$n2 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 1000000; $i++) { $o = new C0(); $n2++; }
$t1 = microtime(true);
echo "SEG new-only        " . round(($t1 - $t0) * 1000, 1) . " ms\n";
$n3 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 1000000; $i++) { $o = new N0(); $o->noop(); $n3++; }
$t1 = microtime(true);
echo "SEG new+noop        " . round(($t1 - $t0) * 1000, 1) . " ms\n";
$n4 = 0;
function freestanding($x) { return $x; }
$t0 = microtime(true);
for ($i = 0; $i < 1000000; $i++) { freestanding($i); $n4++; }
$t1 = microtime(true);
echo "SEG free-call       " . round(($t1 - $t0) * 1000, 1) . " ms\n";
// subtraction anchors (P0-9 call decomposition): empty loop = dispatch
// baseline; nop-call vs free-call isolates arg SEND/RECV machinery
$n5 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 1000000; $i++) { $n5++; }
$t1 = microtime(true);
echo "SEG empty-loop      " . round(($t1 - $t0) * 1000, 1) . " ms\n";
function nopcall() { return 1; }
$n6 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 1000000; $i++) { nopcall(); $n6++; }
$t1 = microtime(true);
echo "SEG nop-call        " . round(($t1 - $t0) * 1000, 1) . " ms\n";
echo "check=$n1/$n2/$n3/$n4/$n5/$n6\n";
