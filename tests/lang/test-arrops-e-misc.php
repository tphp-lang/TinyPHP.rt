<?php
// EXPECT: STEP0
// EXPECT: compound_ok
// EXPECT: o
// EXPECT: abc
echo "STEP0\n";
$k = ["x" => 10];
$k["x"] += 5;
echo "compound_ok\n";
$s = "hello";
echo $s[-1], "\n";
$w = "ab";
$w = $w . "c";
echo $w, "\n";
