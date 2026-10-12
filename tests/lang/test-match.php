<?php
function m($x) {
    return match($x) {
        1, 2 => "small",
        3 => "three",
        default => "big",
    };
}
// EXPECT: string(5) "small"
var_dump(m(2));
// EXPECT: string(5) "three"
var_dump(m(3));
// EXPECT: string(3) "big"
var_dump(m(99));
function m2($x) { return match($x) { 1 => "one", default => "?" }; }
// EXPECT: string(3) "one"
var_dump(m2(1));
// EXPECT: string(1) "?"
var_dump(m2(7));
function m3($n) {
    return match(true) {
        $n < 10 => "lt10",
        $n < 100 => "lt100",
        default => "big",
    };
}
// EXPECT: string(4) "lt10"
var_dump(m3(5));
// EXPECT: string(5) "lt100"
var_dump(m3(50));
// EXPECT: string(3) "big"
var_dump(m3(500));
// match as expression part
// EXPECT: 10
echo match(2) { 1 => 5, 2 => 10, default => 0 }, "\n";
// strict comparison (match uses ===)
function m4($x) { return match($x) { "1" => "str", 1 => "int", default => "?" }; }
// EXPECT: string(3) "int"
var_dump(m4(1));
// EXPECT: string(3) "str"
var_dump(m4("1"));
