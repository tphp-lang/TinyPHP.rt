<?php
// P2-B: array callables [$obj,'m'] / ['Cls','m'] and 'Cls::m' strings —
// both $cb() syntax and call_user_func/array_map/usort paths
class Greeter {
    public $name = "W";
    public function hello($g) { return "hello $g from {$this->name}"; }
    public static function s($n) { return "static $n"; }
}

$g = new Greeter();
$inst = [$g, 'hello'];
var_dump($inst('x'));
$static = ['Greeter', 's'];
var_dump($static(9));
$str = 'Greeter::s';
var_dump($str(11));

// higher-order builtins (tphp_vm_call_callable path)
var_dump(call_user_func([$g, 'hello'], 'cu'));
var_dump(array_map([$g, 'hello'], ['a', 'b']));
$sorted = [3, 1, 2];
class Sorter {
    public static function cmp($x, $y) { return $x <=> $y; }
}
usort($sorted, ['Sorter', 'cmp']);
var_dump($sorted);

// plain function string still works
var_dump(call_user_func('strlen', "abcd"));

// EXPECT: string(14) "hello x from W"
// EXPECT: string(8) "static 9"
// EXPECT: string(9) "static 11"
// EXPECT: string(15) "hello cu from W"
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   string(14) "hello a from W"
// EXPECT:   [1]=>
// EXPECT:   string(14) "hello b from W"
// EXPECT: }
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(1)
// EXPECT:   [1]=>
// EXPECT:   int(2)
// EXPECT:   [2]=>
// EXPECT:   int(3)
// EXPECT: }
// EXPECT: int(4)
