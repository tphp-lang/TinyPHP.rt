<?php
// EXPECT: STEP0
// EXPECT: nested_ok
// EXPECT: cow_ok
echo "STEP0\n";
$g = [[1, 2], [3, 4]];
$h = $g;
$h[0][0] = 77;
echo "nested_ok\n";
$a = [1, 2, 3];
$b = $a;
$b[0] = 99;
echo "cow_ok\n";
