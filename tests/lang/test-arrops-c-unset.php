<?php
// EXPECT: STEP0
// EXPECT: STEP1
// EXPECT: unset_ok
// EXPECT: 2
echo "STEP0\n";
$a = [1, 2, 3];
unset($a[2]);
echo "STEP1\n";
echo "unset_ok\n";
echo count($a), "\n";
