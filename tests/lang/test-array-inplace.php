<?php
// 原地排序/前插（2026-10-15 回归）
// 历史 bug：sort/rsort/array_unshift 重建数组后写回 args[0]（实参副本），
// 对调用方变量是空操作。现在全部原地修改。
// EXPECT: sort=1|2|3
// EXPECT: rsort=3|2|1
// EXPECT: unshift=0|1|2
// EXPECT: strsort=a|b|c
// EXPECT: unshift-keys=k1|k2|2|1|1|2
$c = [3, 1, 2];
sort($c);
echo "sort=", implode("|", $c), "\n";
$d = [1, 3, 2];
rsort($d);
echo "rsort=", implode("|", $d), "\n";
$e = [1, 2];
array_unshift($e, 0);
echo "unshift=", implode("|", $e), "\n";
$s = ["c", "a", "b"];
sort($s);
echo "strsort=", implode("|", $s), "\n";
$m = ["b" => 2, "a" => 1, 1, 2];
array_unshift($m, "k1", "k2");
echo "unshift-keys=", implode("|", $m), "\n";
