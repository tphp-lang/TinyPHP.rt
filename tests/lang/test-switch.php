<?php
function sw($x) {
    $r = "";
    switch ($x) {
        case 1: $r .= "one;";
        case 2: $r .= "two;"; break;
        case 3: $r .= "three;"; break;
        default: $r .= "def;";
    }
    return $r;
}
// EXPECT: string(8) "one;two;"
var_dump(sw(1));
// EXPECT: string(4) "two;"
var_dump(sw(2));
// EXPECT: string(6) "three;"
var_dump(sw(3));
// EXPECT: string(4) "def;"
var_dump(sw(9));
function sw2($x) {
    $r = "";
    switch ($x) {
        default: $r .= "d;";
        case 1: $r .= "1;";
        case 2: $r .= "2;";
    }
    return $r;
}
// EXPECT: string(6) "d;1;2;"
var_dump(sw2(9));
// EXPECT: string(4) "1;2;"
var_dump(sw2(1));
// EXPECT: string(2) "2;"
var_dump(sw2(2));
// string cases
function sw3($x) {
    switch ($x) {
        case "a": return "A";
        case "b": return "B";
    }
    return "?";
}
// EXPECT: string(1) "B"
var_dump(sw3("b"));
// EXPECT: string(1) "?"
var_dump(sw3("z"));
// switch on expression
// EXPECT: string(4) "two;"
var_dump(sw(1 + 1));
