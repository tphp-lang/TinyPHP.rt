<?php
// bench: 1e6 array pushes baseline (PLAN/04 section 2)
// EXPECT: push_count=1000000
// EXPECT: spot_check=ok
// Result is fixed; timing goes to stderr

$t0 = microtime(true);
$a = [];
for ($i = 0; $i < 1000000; $i++) {
    $a[] = $i;
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH push(1e6) = %.1f ms", $ms));

echo "push_count=", count($a), "\n";
$ok = ($a[0] === 0 && $a[499999] === 499999 && $a[999999] === 999999);
echo "spot_check=", $ok ? "ok" : "fail", "\n";
