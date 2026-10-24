<?php
// EXPECT: match=1
// EXPECT: groups=abc,b
// EXPECT: nomatch=0 empty-count=0
// EXPECT: offset=b
// EXPECT: tail-trim=2
// EXPECT: last-err=No error
// EXPECT: ok
// P1: preg_match — 模式/子组/offset/尾未匹配组不插入/last_error

$m = [];
echo "match=", preg_match("/a(b)c/", "xxabcyy", $m), "\n";
echo "groups=", $m[0], ",", $m[1], "\n";

$m = [];
$nm = preg_match("/z/", "abc", $m);
echo "nomatch=", $nm, " empty-count=", count($m), "\n";

$m = [];
preg_match("/b/", "abcabc", $m, 0, 3);
echo "offset=", $m[0], "\n";

// 尾部未参与组默认不插入（PHP 规则）
$m = [];
preg_match("/(a)(z)?/", "ab", $m);
echo "tail-trim=", count($m), "\n";

echo "last-err=", preg_last_error_msg(), "\n";
echo "ok\n";
