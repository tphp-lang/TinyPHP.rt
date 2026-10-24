<?php
// bench: serialize + unserialize nested array x 2e3 (serialization baseline)
// EXPECT: ok2000
$tree = [];
for ($i = 0; $i < 50; $i++) {
    $tree[] = ["id" => $i, "name" => "item$i", "tags" => ["a", "b", "c"]];
}
$t0 = microtime(true);
$n = 0;
for ($i = 0; $i < 2000; $i++) {
    $s = serialize($tree);
    $back = unserialize($s);
    if ($back !== null) $n++;
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH serialize/unserialize 2e3 = %.1f ms", $ms));
echo "ok$n\n";
