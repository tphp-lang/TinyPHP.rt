<?php
// bench: fib(28) recursion baseline (PLAN/04 section 2)
// EXPECT: fib28=317811
// Result is fixed; timing goes to stderr (stdout stays stable for EXPECT)

function fib($n) {
    if ($n < 2) return $n;
    return fib($n - 1) + fib($n - 2);
}

$t0 = microtime(true);
$r = fib(28);
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH fib(28) = %.1f ms", $ms));

echo "fib28=$r\n";
