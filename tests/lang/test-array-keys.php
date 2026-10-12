<?php
$a = [
    1 => "a",
    "1" => "b",     // overwrites 1
    1.5 => "c",     // -> 1, overwrites
    true => "d",    // -> 1, overwrites
    null => "e",    // -> ""
    "08" => "f",    // stays string
    " 8" => "g",    // stays string
    "-3" => "h",    // -> -3
    2.0 => "i",     // -> 2
];
// EXPECT: int(6)
var_dump(count($a));
// EXPECT: string(1) "d"
var_dump($a[1]);
// EXPECT: string(1) "e"
var_dump($a[""]);
// EXPECT: string(1) "h"
var_dump($a[-3]);
// EXPECT: string(1) "i"
var_dump($a[2]);
// EXPECT: string(1) "f"
var_dump($a["08"]);
// EXPECT: string(1) "g"
var_dump($a[" 8"]);
// EXPECT: bool(true)
var_dump(isset($a["08"]));
// EXPECT: bool(false)
var_dump(isset($a["8"]));
$b = [];
$b[] = "x";       // 0
$b["5"] = "y";    // 5
$b[] = "z";       // 6
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   string(1) "x"
// EXPECT:   [5]=>
// EXPECT:   string(1) "y"
// EXPECT:   [6]=>
// EXPECT:   string(1) "z"
// EXPECT: }
var_dump($b);
$c = ["a" => 1];
$c["b"] = 2;
$c[] = 3;         // next_idx: "a"/"b" are strings -> 0
// EXPECT: array(3) {
// EXPECT:   ["a"]=>
// EXPECT:   int(1)
// EXPECT:   ["b"]=>
// EXPECT:   int(2)
// EXPECT:   [0]=>
// EXPECT:   int(3)
// EXPECT: }
var_dump($c);
