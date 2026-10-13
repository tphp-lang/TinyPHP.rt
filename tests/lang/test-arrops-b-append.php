<?php
// EXPECT: STEP0
// EXPECT: STEP1
// EXPECT: count=4
// EXPECT: s=two
echo "STEP0\n";
$a = [1, 2, 3];
$a[] = 4;
echo "STEP1\n";
$a[1] = "two";
echo "count=", count($a), "\n";
echo "s=", $a[1], "\n";
