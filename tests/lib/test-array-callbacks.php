<?php
// EXPECT: map=2,4,6
// EXPECT: map_ids=10,20,30
// EXPECT: sum2=11,22,33
// EXPECT: filter=1|2|a|3
// EXPECT: filter_cb=2|4
// EXPECT: reduce=15
// EXPECT: reduce_init=16
// EXPECT: ok

// array_map
$m = array_map(fn($x) => $x * 2, [1, 2, 3]);
echo "map=", $m[0], ",", $m[1], ",", $m[2], "\n";

$m2 = array_map(fn($x) => $x, [10, 20, 30]);
echo "map_ids=", $m2[0], ",", $m2[1], ",", $m2[2], "\n";

// 多数组
$m3 = array_map(fn($a, $b) => $a + $b, [1, 2, 3], [10, 20, 30]);
echo "sum2=", $m3[0], ",", $m3[1], ",", $m3[2], "\n";

// array_filter 无回调（falsy）
$f = array_filter([0, 1, 2, "", "a", null, 3]);
$parts = [];
foreach ($f as $v) $parts[] = $v;
echo "filter=", implode("|", $parts), "\n";

// array_filter 带回调
$f2 = array_filter([1, 2, 3, 4], fn($x) => $x % 2 === 0);
$parts2 = [];
foreach ($f2 as $v) $parts2[] = $v;
echo "filter_cb=", implode("|", $parts2), "\n";

// array_reduce
$r = array_reduce([1, 2, 3, 4, 5], fn($c, $x) => $c + $x, 0);
echo "reduce=$r\n";

$r2 = array_reduce([1, 2, 3], fn($c, $x) => $c + $x, 10);
echo "reduce_init=$r2\n";
echo "ok\n";
