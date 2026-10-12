<?php
// EXPECT: int(42)
var_dump(42);
// EXPECT: string(5) "hello"
var_dump("hello");
// EXPECT: bool(true)
var_dump(true);
// EXPECT: NULL
var_dump(null);
// EXPECT: float(1.5)
var_dump(1.5);
// EXPECT: float(0)
var_dump(0.0);
// EXPECT: int(9223372036854775807)
var_dump(PHP_INT_MAX);
// EXPECT: int(-9223372036854775808)
var_dump(PHP_INT_MIN);
// EXPECT: int(8)
var_dump(PHP_INT_SIZE);
// EXPECT: float(INF)
var_dump(INF);
// EXPECT: float(NAN)
var_dump(NAN);
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   ["a"]=>
// EXPECT:   array(1) {
// EXPECT:     [0]=>
// EXPECT:     bool(true)
// EXPECT:   }
// EXPECT: }
var_dump([1, "a" => [true]]);
// EXPECT: array(0) {
// EXPECT: }
var_dump([]);
// EXPECT: string(8) "tabs	and"
var_dump("tabs\tand");
