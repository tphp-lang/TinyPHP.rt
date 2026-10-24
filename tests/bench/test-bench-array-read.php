<?php
// bench: packed int-array sequential read x 1e6 (array read baseline)
// EXPECT: sum=499500000
$a = [];
for ($i = 0; $i < 1000; $i++) $a[] = $i;
$t0 = microtime(true);
$sum = 0;
for ($r = 0; $r < 1000; $r++) {
    for ($i = 0; $i < 1000; $i++) $sum += $a[$i];
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH array read 1e6 = %.1f ms", $ms));
echo "sum=$sum\n";
