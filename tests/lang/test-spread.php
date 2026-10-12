<?php
$src = [1, 2, 3];
$out = [0, ...$src, 4];
// EXPECT: array(5) {
// EXPECT:   [0]=>
// EXPECT:   int(0)
// EXPECT:   [1]=>
// EXPECT:   int(1)
// EXPECT:   [2]=>
// EXPECT:   int(2)
// EXPECT:   [3]=>
// EXPECT:   int(3)
// EXPECT:   [4]=>
// EXPECT:   int(4)
// EXPECT: }
var_dump($out);
$kv = ["a" => 1];
$out2 = [...$kv, "b" => 2];
// EXPECT: array(2) {
// EXPECT:   ["a"]=>
// EXPECT:   int(1)
// EXPECT:   ["b"]=>
// EXPECT:   int(2)
// EXPECT: }
var_dump($out2);
$one = [1];
$two = [2];
$both = [...$one, ...$two];
// EXPECT: int(2)
var_dump(count($both));
// spread takes a snapshot, source untouched
$snap = [1, 2];
$copy = [...$snap];
$copy[0] = 9;
// EXPECT: 1
echo $snap[0], "\n";
// EXPECT: 9
echo $copy[0], "\n";
// spread with keys overwriting
$ow = ["a" => 1];
$out3 = [...$ow, "a" => 2];
// EXPECT: int(2)
var_dump($out3["a"]);
