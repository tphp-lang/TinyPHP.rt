<?php
// EXPECT: named=hello
// EXPECT: unmatched-null=a,null
// EXPECT: offset-cap=bc,1
// EXPECT: all-pattern=a,b a,b
// EXPECT: all-set=2
// EXPECT: empty-g=3
// EXPECT: ok
// P2: preg_match 命名组 / UNMATCHED_AS_NULL / OFFSET_CAPTURE + preg_match_all

// 命名组
$m = [];
preg_match("/(?P<word>\w+)/", " hello world", $m);
echo "named=", $m["word"], "\n";

// UNMATCHED_AS_NULL=512：尾部未参与组补 null（而非截断）；组1已匹配="a"
$m = [];
preg_match("/(a)(z)?/", "a", $m, 512);
echo "unmatched-null=", ($m[1] === null ? "null" : $m[1]), ",",
     ($m[2] === null ? "null" : $m[2]), "\n";

// OFFSET_CAPTURE=256
$m = [];
preg_match("/b(c)/", "abc", $m, 256);
echo "offset-cap=", $m[0][0], ",", $m[0][1], "\n";

// match_all PATTERN_ORDER（默认）
$ma = [];
preg_match_all("/(\w)(\w)/", "aa bb", $ma);
echo "all-pattern=", $ma[1][0], ",", $ma[1][1], " ", $ma[2][0], ",", $ma[2][1], "\n";

// match_all SET_ORDER=1
$ms = [];
$n = preg_match_all("/\w+/", "one two", $ms, 1);
echo "all-set=", $n, "\n";

// 空匹配推进（/g 语义）
$me = [];
preg_match_all("/a*/", "aab", $me);
echo "empty-g=", count($me[0]), "\n";
echo "ok\n";
