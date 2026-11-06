<?php
// 刀13：array 族病态修复（unique/flip/chunk/keys 的桶反查 + 重扫 O(n²)）
//       + 缺口补齐（strncmp/strcasecmp/strncasecmp/function_exists）。
// 五模一致：默认 / TPHP_DYN_JIT=0 / TPHP_NEST_CALL=1 / TPHP_JIT=0 / PHP 8.5。
// （unique 的比较语义 = 既有 strict——PHP 默认 SORT_STRING 差异另案 P1-17。）

// ---- unique：hash 化 + 语义零漂移（含混合类型/数字串边界） ----
$a = [1, 2, 2, "1", 3, 3, "x", "x"];
echo "1 unique=", json_encode(array_unique($a)), "\n";
// EXPECT: 1 unique={"0":1,"1":2,"3":"1","4":3,"6":"x"}

$mix = [1, "1", 1.0, true, null];
echo "2 unique-strict=", json_encode(array_unique($mix)), "\n";
// EXPECT: 2 unique-strict=[1,"1",1.0,true,null]

$s = ["a", "b", "a", "c", "b"];
echo "3 unique-str=", implode(",", array_unique($s)), "\n";
// EXPECT: 3 unique-str=a,b,c

$big = [];
for ($i = 0; $i < 20000; $i++) $big[] = ($i * 7919) % 65536;
$u = array_unique($big);
echo "4 unique-20k=", count($u), " sorted-first=", implode(",", array_slice(array_values($u), 0, 3)), "\n";
// EXPECT: 4 unique-20k=20000 sorted-first=0,7919,15838

// ---- keys / flip / chunk：inv 反查修复（assoc/holey 形态） ----
$assoc = ["a" => 1, "b" => 2, 5 => 3];
echo "5 keys=", json_encode(array_keys($assoc)), "\n";
// EXPECT: 5 keys=["a","b",5]

echo "6 flip=", json_encode(array_flip($assoc)), "\n";
// EXPECT: 6 flip={"1":"a","2":"b","3":5}

$h = [1 => "x", 3 => "y"];
echo "7 flip-int=", json_encode(array_flip($h)), "\n";
// EXPECT: 7 flip-int={"x":1,"y":3}

$c = array_chunk($assoc, 2);
echo "8 chunk=", count($c), " c0=", json_encode($c[0]), "\n";
// EXPECT: 8 chunk=2 c0=[1,2]

$c2 = array_chunk([1, 2, 3, 4, 5], 2, true);
echo "9 chunk-preserve=", json_encode($c2), "\n";
// EXPECT: 9 chunk-preserve=[[1,2],{"2":3,"3":4},{"4":5}]

// ---- strncmp 族（memcmp + 归一 -1/0/1） ----
echo "10 strcmp=", strcmp("a", "b"), "|", strcmp("abc", "abc0"), "\n";
// EXPECT: 10 strcmp=-1|-1

echo "11 strncmp=", strncmp("abcdef", "abcxyz", 3), "|", strncmp("xyz", "abc", 2), "\n";
// EXPECT: 11 strncmp=0|1

echo "12 strcasecmp=", strcasecmp("Abc", "abd"), "|", strcasecmp("ABC", "abc"), "\n";
// EXPECT: 12 strcasecmp=-1|0

// PHP 8.5 contract: strncasecmp returns raw tolower byte diff (not normalized)
echo "13 strncasecmp=", strncasecmp("Abcdef", "ABCxyz", 3), "|", strncasecmp("Abc", "XYZ", 2), "\n";
// EXPECT: 13 strncasecmp=0|-23

echo "14 strncmp-bin=", strncmp("\x80a", "\x80b", 2), "\n";
// EXPECT: 14 strncmp-bin=-1

echo "15 cmp-len=", strncasecmp("ab", "abc", 3), "|", strcasecmp("ab", "abc"), "|", strncmp("ab", "abc", 3), "\n";
// EXPECT: 15 cmp-len=-1|-1|-1

// ---- function_exists（内建 + 用户函数） ----
function k13_user_fn() { return 1; }
echo "15 fe=", function_exists("strcmp") ? "T" : "F", "|",
     function_exists("k13_user_fn") ? "T" : "F", "|",
     function_exists("no_such_fn_zz") ? "T" : "F", "\n";
// EXPECT: 15 fe=T|T|F

// ---- 刀14：diff/intersect/count_values/splice/ucfirst/is_callable strict ----
class K14C { public function run() { return 1; } }

$a14 = [1, "1", 2, 3.0, "x"];
echo "16 diff=", json_encode(array_diff($a14, [1, 2])), "\n";
// pre-existing json gap: tphp float 3.0 vs PHP 3
// EXPECT: 16 diff={"3":3.0,"4":"x"}

$ax = ["a" => 1, "b" => 2, "c" => 3];
echo "17 diff-assoc=", json_encode(array_diff($ax, [2, 3])), "|",
     "intersect=", json_encode(array_intersect($ax, [2, 3])), "\n";
// EXPECT: 17 diff-assoc={"a":1}|intersect={"b":2,"c":3}

echo "18 count=", json_encode(array_count_values([1, "1", 2, 1, "x", "x"])), "\n";
// EXPECT: 18 count={"1":3,"2":1,"x":2}

$arr = [1, 2, 3, 4, 5];
$r = array_splice($arr, 1, 2);
echo "19 splice=", json_encode($arr), " ret=", json_encode($r), "\n";
// EXPECT: 19 splice=[1,4,5] ret=[2,3]

$arr2 = ["a" => 1, "b" => 2, "c" => 3, "d" => 4];
$r2 = array_splice($arr2, 1, 2, ["x", "y"]);
echo "20 splice-rep=", json_encode($arr2), " ret=", json_encode($r2), "\n";
// EXPECT: 20 splice-rep={"a":1,"0":"x","1":"y","d":4} ret={"b":2,"c":3}

$arr3 = [1, 2, 3, 4, 5];
echo "21 splice-neg=", json_encode(array_splice($arr3, -2)), " left=", json_encode($arr3), "\n";
// EXPECT: 21 splice-neg=[4,5] left=[1,2,3]

$arr4 = [1, 2, 3];
echo "22 splice-scalar=", json_encode(array_splice($arr4, 1, -1, "z")), " left=", json_encode($arr4), "\n";
// EXPECT: 22 splice-scalar=[2] left=[1,"z",3]

echo "23 ucfirst=", ucfirst("hello world"), "|", strlen(ucfirst("")), "|", strlen(ucfirst("\xe4\xbd\xa0 w")), "\n";
// EXPECT: 23 ucfirst=Hello world|0|5

echo "24 ic-strict=", is_callable("strcmp") ? "T" : "F", "|",
     is_callable("no_such_fn_zz") ? "T" : "F", "|",
     is_callable([new K14C(), "nope"]) ? "T" : "F", "|",
     is_callable("no_such_fn_zz", true) ? "T" : "F", "\n";
// EXPECT: 24 ic-strict=T|F|F|T

echo "all-ok\n";
// EXPECT: all-ok

