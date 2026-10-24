<?php
// bench: json_encode + json_decode x 2e3 (JSON baseline)
// EXPECT: ok2000
$rows = [];
for ($i = 0; $i < 100; $i++) {
    $rows[] = ["id" => $i, "name" => "row$i", "ok" => true, "score" => 1.5];
}
$t0 = microtime(true);
$n = 0;
for ($i = 0; $i < 2000; $i++) {
    $j = json_encode($rows);
    $back = json_decode($j);
    if ($back !== null) $n++;
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH json enc+dec 2e3 = %.1f ms", $ms));
echo "ok$n\n";
