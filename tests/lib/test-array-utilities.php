<?php
// EXPECT: sorted=1|1|2|3|4|5
// EXPECT: rev=3|2|1
// EXPECT: flip_val=a
// EXPECT: unique=1|2|3
// EXPECT: search=1
// EXPECT: search_miss=0
// EXPECT: kf=x
// EXPECT: kl=z
// EXPECT: range_i=1|2|3|4|5
// EXPECT: range_c=a|b|c
// EXPECT: chr=A
// EXPECT: ord=65
// EXPECT: ok

$a = [3, 1, 4, 1, 5, 2];
usort($a, fn($x, $y) => $x - $y);
$pa = [];
foreach ($a as $v) $pa[] = $v;
echo "sorted=", implode("|", $pa), "\n";

$b = [1, 2, 3];
usort($b, fn($x, $y) => $y - $x);
$pb = [];
foreach ($b as $v) $pb[] = $v;
echo "rev=", implode("|", $pb), "\n";

$fl = array_flip(["a" => 1]);
echo "flip_val=", $fl[1], "\n";

$u = array_unique([1, 2, 2, 3, 3, 3]);
$pu = [];
foreach ($u as $v) $pu[] = $v;
echo "unique=", implode("|", $pu), "\n";

$s = array_search(2, [1, 2, 3]);
echo "search=$s\n";
$sf = array_search(99, [1, 2, 3]);
echo "search_miss=", $sf === false ? 0 : 1, "\n";

$arr = ["x" => 10, "y" => 20, "z" => 30];
echo "kf=", array_key_first($arr), "\n";
echo "kl=", array_key_last($arr), "\n";

$r = range(1, 5);
$pr = [];
foreach ($r as $v) $pr[] = $v;
echo "range_i=", implode("|", $pr), "\n";

$rc = range("a", "c");
$pc = [];
foreach ($rc as $v) $pc[] = $v;
echo "range_c=", implode("|", $pc), "\n";

echo "chr=", chr(65), "\n";
echo "ord=", ord("A"), "\n";
echo "ok\n";
