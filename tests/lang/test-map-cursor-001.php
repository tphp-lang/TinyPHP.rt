<?php
// 刀11：array_map O(n²) 病态修复（每源游标推进）+ strcmp() 补齐（P1-15）。
// 刀12：array_map 语义对齐（多数组短源 null 填充至最长 + 单数组键保持）+
//       array_filter 键反查 O(n²) 修复（inv[] 反查 + packed 快路）。
// 五模逐字一致：默认 / TPHP_DYN_JIT=0 / TPHP_NEST_CALL=1 / TPHP_JIT=0 / PHP 8.5。

function raise_it($m) { throw new Exception($m); }

// ---- 游标化等价性（刀10 语义保持：内部 catch / 逃逸） ----
$a = [1, 2, 3, 4];
echo "1 single=", implode(",", array_map(function ($x) { return $x * 2; }, $a)), "\n";
// EXPECT: 1 single=2,4,6,8

$x = [1, 2, 3];
$y = [10, 20, 30];
echo "2 two=", implode("|", array_map(function ($p, $q) { return $p + $q; }, $x, $y)), "\n";
// EXPECT: 2 two=11|22|33

$holey = [1, 2, 3, 4, 5];
unset($holey[1]);
unset($holey[3]);   // UNDEF 洞：按插入序跳过（游标跳洞语义）
echo "3 holey=", implode(",", array_map(function ($v) { return $v * 3; }, $holey)), "\n";
// EXPECT: 3 holey=3,9,15

$holey2 = [5, 6, 7, 8, 9];
unset($holey2[0]);
unset($holey2[2]);
echo "4 two-holey=", implode("|", array_map(function ($p, $q) { return "$p$q"; }, $holey, $holey2)), "\n";
// EXPECT: 4 two-holey=16|38|59

$assoc = ["a" => 1, "b" => 2];
echo "5 assoc=", implode(",", array_map(function ($v) { return $v + 100; }, $assoc)), "\n";
// EXPECT: 5 assoc=101,102

echo "6 empty=", count(array_map(function ($v) { return $v; }, [])), "\n";
// EXPECT: 6 empty=0

// 内部 catch（刀10 语义：不挂、排序/map 继续）
$m = array_map(function ($x) { try { if ($x === 2) raise_it("in-map"); } catch (Exception $e) { } return $x * 10; }, [1, 2, 3]);
echo "7 inner-catch=", implode(",", $m), "\n";
// EXPECT: 7 inner-catch=10,20,30

try { array_map(function ($v) { raise_it("esc8"); return $v; }, [1, 2]); echo "8 MISS\n"; }
catch (Exception $e) { echo "8 escape=", $e->getMessage(), "\n"; }
// EXPECT: 8 escape=esc8

// ---- strcmp（P1-15） ----
echo "9 strcmp=", strcmp("a", "b"), "|", strcmp("b", "a"), "|", strcmp("a", "a"), "\n";
// EXPECT: 9 strcmp=-1|1|0

echo "10 strcmp-len=", strcmp("abc", "abc0"), "|", strcmp("abc0", "abc"), "|", strcmp("", ""), "\n";
// EXPECT: 10 strcmp-len=-1|1|0

echo "11 strcmp-bin=", strcmp("\x80\x01", "\x7f\xff"), "\n";   // 二进制安全（无符号字节序）
// EXPECT: 11 strcmp-bin=1

$w = ["bb", "a", "ccc", "dd"];
usort($w, function ($p, $q) { return strlen($p) - strlen($q) ?: strcmp($p, $q); });
echo "12 usort-str=", implode(",", $w), "\n";
// EXPECT: 12 usort-str=a,bb,dd,ccc

// ---- 刀12：map 语义对齐（PHP 8.5 实测基准） ----
$x = [1, 2, 3];
$short = [7, 8];
echo "13 short-null=", implode("|", array_map(function ($p, $q) { return "$p:" . ($q === null ? "N" : $q); }, $x, $short)), "\n";
// EXPECT: 13 short-null=1:7|2:8|3:N

$assoc = ["a" => 1, "b" => 2];
$r13 = array_map(function ($v) { return $v * 2; }, $assoc);
echo "14 single-assoc=", json_encode(array_keys($r13)), " v=", implode(",", $r13), "\n";
// EXPECT: 14 single-assoc=["a","b"] v=2,4

$ax = ["a" => 1, "b" => 2];
$ay = ["c" => 10, "d" => 20];
$r14 = array_map(function ($p, $q) { return $p + $q; }, $ax, $ay);
echo "15 multi-assoc=", json_encode(array_keys($r14)), "\n";
// EXPECT: 15 multi-assoc=[0,1]

$holey = [1, 2, 3, 4];
unset($holey[1]);
$r15 = array_map(function ($v) { return $v * 3; }, $holey);
echo "16 holey-keys=", json_encode(array_keys($r15)), " v=", implode(",", $r15), "\n";
// EXPECT: 16 holey-keys=[0,2,3] v=3,9,12

$mix = ["k" => 1, 5, "j" => 2, 6];
$r16 = array_map(function ($v) { return $v; }, $mix);
echo "17 mixed-keys=", json_encode(array_keys($r16)), "\n";
// EXPECT: 17 mixed-keys=["k",0,"j",1]

// ---- 刀12：filter 键保持（packed 快路 + inv 反查双形态） ----
$fass = ["a" => 1, "b" => 2, "c" => 3, "d" => 4];
$r17 = array_filter($fass, function ($v) { return $v & 1; });
echo "18 filter-assoc=", json_encode($r17), "\n";
// EXPECT: 18 filter-assoc={"a":1,"c":3}

$fh = [1, 2, 3, 4];
unset($fh[1]);
$r18 = array_filter($fh);
echo "19 filter-holey=", json_encode($r18), "\n";
// EXPECT: 19 filter-holey={"0":1,"2":3,"3":4}

echo "all-ok\n";
// EXPECT: all-ok
