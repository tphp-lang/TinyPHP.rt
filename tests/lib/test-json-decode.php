<?php
// EXPECT: NULL
// EXPECT: bool(true)
// EXPECT: bool(false)
// EXPECT: int(42)
// EXPECT: float(3.5)
// EXPECT: string(5) "hello"
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   int(2)
// EXPECT:   [2]=>
// EXPECT:   int(3)
// EXPECT: }
// EXPECT: array(1) {
// EXPECT:   ["a"]=>
// EXPECT:   int(9)
// EXPECT: }
// EXPECT: ok

var_dump(json_decode("null"));
var_dump(json_decode("true"));
var_dump(json_decode("false"));
var_dump(json_decode("42"));
var_dump(json_decode("3.5"));
var_dump(json_decode('"hello"'));
var_dump(json_decode("[1,2,3]"));
var_dump(json_decode('{"a":9}'));
echo "ok\n";
