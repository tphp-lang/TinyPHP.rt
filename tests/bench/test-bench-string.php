<?php
// bench: string concat + strpos + substr x 5e4 (string-heavy workload)
// EXPECT: len=400000 pos=5 sub=def
$base = "abcdefghij";
$t0 = microtime(true);
$s = "";
for ($i = 0; $i < 40000; $i++) $s .= $base;
$pos = strpos($s, "fghij");
$sub = substr($s, 3, 3);
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH string concat4e4+find = %.1f ms", $ms));
echo "len=", strlen($s), " pos=", $pos, " sub=", $sub, "\n";
