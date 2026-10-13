<?php
// EXPECT: STEP0
// EXPECT: STEP1
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   int(2)
// EXPECT:   [2]=>
// EXPECT:   int(3)
// EXPECT: }
echo "STEP0\n";
$a = [1, 2, 3];
echo "STEP1\n";
var_dump($a);
