<?php
// bench: throw+catch x 1e5 (exception path baseline)
// EXPECT: caught=100000 last=v
class BenchEx extends Exception {}

$t0 = microtime(true);
$n = 0;
$last = "";
for ($i = 0; $i < 100000; $i++) {
    try {
        throw new BenchEx("v");
    } catch (BenchEx $e) {
        $n++;
        $last = $e->getMessage();
    }
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH throw/catch 1e5 = %.1f ms", $ms));
echo "caught=$n last=$last\n";
