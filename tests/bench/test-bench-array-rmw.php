<?php
// bench: array read-modify-write 1e5 baseline
// EXPECT: rmw=ok
// Result is fixed; timing goes to stderr

$n = 100000;
$t0 = microtime(true);
$a = array_fill(0, $n, 0);
for ($i = 0; $i < $n; $i++) {
    $a[$i] = $a[$i] + 1;
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH rmw(1e5) = %.1f ms", $ms));

$ok = true;
for ($i = 0; $i < $n; $i += 10000) {
    if ($a[$i] !== 1) { $ok = false; break; }
}
echo "rmw=", $ok ? "ok" : "fail", "\n";
