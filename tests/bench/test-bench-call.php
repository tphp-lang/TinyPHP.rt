<?php
// bench: empty function + method call x 1e6 (call overhead baseline)
// EXPECT: fcalls=0 mcalls=0
function noop($x) { return $x; }
class Box { public function id($x) { return $x; } }

$b = new Box();
$t0 = microtime(true);
$s = 0;
for ($i = 0; $i < 1000000; $i++) $s = noop($s);
$m = 0;
for ($i = 0; $i < 1000000; $i++) $m = $b->id($m);
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH call 1e6 fn+method = %.1f ms", $ms));
echo "fcalls=$s mcalls=$m\n";
