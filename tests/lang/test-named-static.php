<?php
// P2-A: named args on static/self calls and new (compile-time signatures).
// Instance-method named args stay rejected (receiver type unknown at compile
// time) — documented in TODO-P2-P3.md.
class Pt {
    public int $x;
    public int $y;
    public function __construct($x = 0, $y = 0) { $this->x = $x; $this->y = $y; }
    public static function make($a = 1, $b = 2) { return [$a, $b]; }
    public static function mix($req, $opt = 9) { return [$req, $opt]; }
}
class Mk {
    public static function build(): array { return Pt::make(b: 30); }
}

var_dump(Pt::make(b: 20));
var_dump(Pt::make(5, b: 6));
var_dump(Pt::mix(7));
var_dump(Mk::build());
$p = new Pt(y: 44);
var_dump($p->x);
var_dump($p->y);
$p2 = new Pt(1, y: 5);
var_dump($p2->x);
var_dump($p2->y);
var_dump(Pt::make(a: 9));

// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   int(20)
// EXPECT: }
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(5)
// EXPECT:   [1]=>
// EXPECT:   int(6)
// EXPECT: }
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(7)
// EXPECT:   [1]=>
// EXPECT:   int(9)
// EXPECT: }
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   int(30)
// EXPECT: }
// EXPECT: int(0)
// EXPECT: int(44)
// EXPECT: int(1)
// EXPECT: int(5)
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(9)
// EXPECT:   [1]=>
// EXPECT:   int(2)
// EXPECT: }
