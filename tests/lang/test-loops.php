<?php
$s = 0;
for ($i = 1; $i <= 10; $i++) $s += $i;
// EXPECT: 55
echo $s, "\n";
$n = 0;
while (true) { $n++; if ($n >= 5) break; }
// EXPECT: 5
echo $n, "\n";
$s = 0;
for ($i = 0; $i < 10; $i++) { if ($i % 2) continue; $s += $i; }
// EXPECT: 20
echo $s, "\n";
$n = 10;
do { $n++; } while ($n < 5);
// EXPECT: 11
echo $n, "\n";
$count = 0;
for ($i = 0; $i < 3; $i++) {
    for ($j = 0; $j < 3; $j++) {
        if ($j == 1) break 2;
        $count++;
    }
}
// EXPECT: int(1)
var_dump($count);
$out = 0;
for ($i = 0; $i < 3; $i++) {
    for ($j = 0; $j < 3; $j++) {
        if ($j == 1) continue 2;
        $out++;
    }
}
// EXPECT: int(3)
var_dump($out);
$t = 0;
foreach ([10, 20, 30] as $v) $t += $v;
// EXPECT: 60
echo $t, "\n";
$r = "";
foreach (["a" => 1, "b" => 2] as $k => $v) $r .= "$k$v;";
// EXPECT: a1;b2;
echo $r, "\n";
$arr = [1, 2, 3];
$sum = 0;
foreach ($arr as $v) { $arr[] = $v * 10; $sum += $v; }
// EXPECT: 6
echo $sum, "\n";
// EXPECT: int(6)
var_dump(count($arr));
// foreach with unset keys
$m = [1 => "x", 5 => "y", 7 => "z"];
$seq = "";
foreach ($m as $v) $seq .= $v;
// EXPECT: xyz
echo $seq, "\n";
$keys = "";
foreach ($m as $kk => $vv) $keys .= "$kk;";
// EXPECT: 1;5;7;
echo $keys, "\n";
