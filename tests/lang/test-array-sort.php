<?php
// asort/arsort/ksort/krsort（P1-11）：键保持排序 + flags（SORT_NUMERIC/
// STRING/FLAG_CASE）+ 稳定性（PHP 8.0+）+ 混合键序（int 在前）。全部用例
// 与 PHP 8.5 逐字节对拍后固化。
$a = ["b" => 3, "a" => 1, "c" => 2];
asort($a);
echo json_encode($a), "\n";
// EXPECT: {"a":1,"c":2,"b":3}
$a = ["b" => 3, "a" => 1, "c" => 2];
arsort($a);
echo json_encode($a), "\n";
// EXPECT: {"b":3,"c":2,"a":1}
$m = [3, 1, 2];                       // int 键跟值走
asort($m);
echo json_encode($m), "\n";
// EXPECT: {"1":1,"2":2,"0":3}
$m = ["x" => 10, "a" => 2, "m" => 7];
ksort($m);
echo json_encode($m), "\n";
// EXPECT: {"a":2,"m":7,"x":10}
$m = ["x" => 10, "a" => 2, "m" => 7];
krsort($m);
echo json_encode($m), "\n";
// EXPECT: {"x":10,"m":7,"a":2}
$m = [10 => "b", 2 => "c", 7 => "a"];
ksort($m);
echo json_encode($m), "\n";
// EXPECT: {"2":"c","7":"a","10":"b"}
// 混合 int/string 键：int 在前（数字键序），string 键字母序
$m = ["b" => 1, 10 => 2, "a" => 3, 2 => 4];
ksort($m);
echo json_encode($m), "\n";
// EXPECT: {"2":4,"10":2,"a":3,"b":1}
// flags: NUMERIC 数值序 vs STRING 字节序
$m = ["10" => "a", "9" => "b", "100" => "c"];
asort($m, SORT_NUMERIC);
echo json_encode(array_values($m)), "\n";
// EXPECT: ["a","b","c"]
$m = ["10" => "a", "9" => "b", "100" => "c"];
asort($m, SORT_STRING);
echo json_encode(array_values($m)), "\n";
// EXPECT: ["a","b","c"]
// FLAG_CASE：Apple < applesauce < banana（不区分大小写）
$m = ["Apple" => 1, "banana" => 2, "applesauce" => 3];
asort($m, SORT_STRING | SORT_FLAG_CASE);
echo json_encode(array_keys($m)), "\n";
// EXPECT: ["Apple","banana","applesauce"]
// 稳定性：等值保持插入序（PHP 8.0+）
$m = ["a" => 1, "b" => 1, "c" => 1, "d" => 0];
asort($m);
echo json_encode(array_keys($m)), "\n";
// EXPECT: ["d","a","b","c"]
// 返回 true / 空数组 / 单元素
$e = [];
var_dump(asort($e));
// EXPECT: bool(true)
$one = ["z" => 9];
var_dump(ksort($one));
// EXPECT: bool(true)
echo json_encode($one), "\n";
// EXPECT: {"z":9}
// SORT_REGULAR 混合类型（数值串感知：9 < 9.5 < "10"）
$m = ["s" => "10", "f" => 9.5, "i" => 9];
asort($m);
echo json_encode($m), "\n";
// EXPECT: {"i":9,"f":9.5,"s":"10"}
echo "sort-done\n";
// EXPECT: sort-done
