<?php
/* B1-3 value probe (PLAN/osr.md s9.5): search-with-early-exit hot loop --
 * the shape B1 unlocks. Function-level JIT: the whole search loop compiles
 * (pre-B1 it ran interpreted 100%). JIT=1 vs JIT=0, min-of-N via
 * bench-rounds.ps1 (script prints one BENCH line per mode tag). */
function bench($name, $fn) {
    $t0 = microtime(true);
    $fn();
    $ms = (microtime(true) - $t0) * 1000;
    error_log(sprintf("BENCH %-18s %8.1f ms", $name, $ms));
}

function search_exit($a, $n, $target) {
    $i = 0;
    for (; $i < $n; $i++) {
        if ($a[$i] === $target) break;
    }
    return $i;
}

$n = 1000000;
$a = [];
for ($i = 0; $i < $n; $i++) { $a[] = $i * 7 % 1000003; }
/* break fires at i=142857: 999999 = 7*142857 (mod-1000003 image), i.e. a
 * ~143k-iteration hot loop whose early exit runs through the B1 path */
$a[$n - 2] = 999999;

/* warm-up: cross TPHP_JIT_HOT_THRESHOLD (127) so the eager face compiles
 * before the timed region (JIT=0 runs the same calls interpreted) */
for ($w = 0; $w < 160; $w++) { search_exit($a, 64, 999999); }

function run_rep($a, $n, $rep) {
    $t = 0;
    for ($r = 0; $r < $rep; $r++) {
        $t += search_exit($a, $n, 999999);
    }
    return $t;
}

$rep = 20;
bench("break-search", function () use ($a, $n, $rep) {
    return run_rep($a, $n, $rep);
});
echo "hit=", run_rep($a, $n, $rep), "\n";
