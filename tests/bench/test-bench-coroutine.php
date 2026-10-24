<?php
// bench: spawn+await round-trips x 1e4 (coroutine switch baseline)
// EXPECT: done=10000 sum=49995000
use function Tphp\spawn;
use function Tphp\await;

$t0 = microtime(true);
$sum = 0;
for ($i = 0; $i < 10000; $i++) {
    $c = spawn(function () use ($i) { return $i; });
    $sum += await($c);
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH spawn/await 1e4 = %.1f ms", $ms));
echo "done=10000 sum=$sum\n";
