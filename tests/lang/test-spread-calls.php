<?php
// P2-A: spread args on method/static calls (new stays rejected: op_new_user
// locates ctor args via compile-time a2, wrong once spread expands)
class C {
    public function m(...$xs): array { return $xs; }
    public static function s(...$xs): array { return $xs; }
}
$c = new C();
var_dump($c->m(1, 2));
$args = [3, 4];
var_dump($c->m(...$args));
var_dump($c->m(1, ...$args));
var_dump(C::s(...[5]));
var_dump(C::s(7, ...[8, 9]));
$xs = [];
var_dump($c->m(...$xs));

// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   int(2)
// EXPECT: }
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   int(3)
// EXPECT:   [1]=>
// EXPECT:   int(4)
// EXPECT: }
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   int(3)
// EXPECT:   [2]=>
// EXPECT:   int(4)
// EXPECT: }
// EXPECT: array(1) {
// EXPECT:   [0]=>
// EXPECT:   int(5)
// EXPECT: }
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(7)
// EXPECT:   [1]=>
// EXPECT:   int(8)
// EXPECT:   [2]=>
// EXPECT:   int(9)
// EXPECT: }
// EXPECT: array(0) {
// EXPECT: }
