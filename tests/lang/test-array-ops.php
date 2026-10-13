<?php
// EXPECT: STEP0
// EXPECT: STEP1
// EXPECT: STEP2
// EXPECT: array(4) {
echo "STEP0\n";
$a = [1, 2, 3];
echo "STEP1\n";
$a[] = 4;
$a[1] = "two";
echo "STEP2\n";
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   string(3) "two"
// EXPECT:   [2]=>
// EXPECT:   int(3)
// EXPECT:   [3]=>
// EXPECT:   int(4)
// EXPECT: }
var_dump($a);
// EXPECT: 4
echo count($a), "\n";
unset($a[2]);
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   string(3) "two"
// EXPECT:   [3]=>
// EXPECT:   int(4)
// EXPECT: }
var_dump($a);
$a[] = 5;
// EXPECT: 4
echo count($a), "\n";
// EXPECT: array(4) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   string(3) "two"
// EXPECT:   [3]=>
// EXPECT:   int(4)
// EXPECT:   [4]=>
// EXPECT:   int(5)
// EXPECT: }
var_dump($a);
// ---- copy-on-write ----
$b = $a;
$b[0] = 99;
// EXPECT: 1
echo $a[0], "\n";
// EXPECT: 99
echo $b[0], "\n";
// nested COW
$g = [[1, 2], [3, 4]];
$h = $g;
$h[0][0] = 77;
// EXPECT: 1
echo $g[0][0], "\n";
// EXPECT: 77
echo $h[0][0], "\n";
// string offsets
$s = "hello";
// EXPECT: h
echo $s[0], "\n";
// EXPECT: o
echo $s[-1], "\n";
// compound on dim
$k = ["x" => 10];
$k["x"] += 5;
// EXPECT: int(15)
var_dump($k["x"]);
$w = "ab";
$w = $w . "c";
// EXPECT: abc
echo $w, "\n";
// isset
// EXPECT: bool(true)
var_dump(isset($a[3]));
// EXPECT: bool(false)
var_dump(isset($a[2]));
// EXPECT: bool(true)
var_dump(isset($k["x"]));
