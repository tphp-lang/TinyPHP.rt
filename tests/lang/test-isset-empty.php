<?php
$x = 0;
$y = "";
$z = null;
$arr = ["k" => ""];
// EXPECT: bool(true)
var_dump(isset($x));
// EXPECT: bool(true)
var_dump(isset($arr["k"]));
// EXPECT: bool(false)
var_dump(isset($arr["no"]));
// EXPECT: bool(false)
var_dump(isset($nope));
// EXPECT: bool(false)
var_dump(isset($nope, $x));
// EXPECT: bool(true)
var_dump(isset($x, $y));
// EXPECT: bool(false)
var_dump(isset($arr["no"], $x));
// EXPECT: bool(true)
var_dump(empty($nope));
// EXPECT: bool(true)
var_dump(empty($z));
// EXPECT: bool(true)
var_dump(empty(""));
// EXPECT: bool(true)
var_dump(empty("0"));
// EXPECT: bool(false)
var_dump(empty("0.0"));
// EXPECT: bool(true)
var_dump(empty(0));
// EXPECT: bool(true)
var_dump(empty([]));
// EXPECT: bool(false)
var_dump(empty([1]));
// EXPECT: int(1)
var_dump(empty($x) ? 1 : 0);
// EXPECT: bool(true)
var_dump(empty($arr["no"]));
// EXPECT: bool(true)
var_dump(empty($arr["k"]));
