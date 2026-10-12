<?php
// var_dump 对象渲染 + __debugInfo + enum case
// EXPECT: object(P)#1 (2) {
// EXPECT:   ["x"]=>
// EXPECT:   int(1)
// EXPECT:   ["y"]=>
// EXPECT:   string(1) "s"
// EXPECT: }
// EXPECT: object(D)#2 (1) {
// EXPECT:   ["shown"]=>
// EXPECT:   string(3) "yes"
// EXPECT: }
// EXPECT: enum(E::Hearts)

class P {
    public $x = 1;
    private $y = "s";
}

class D {
    public $hidden = "no";
    public function __debugInfo(): array {
        return ["shown" => "yes"];
    }
}

enum E: string {
    case Hearts = "H";
}

var_dump(new P());
var_dump(new D());
var_dump(E::Hearts);
