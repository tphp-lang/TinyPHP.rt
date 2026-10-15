<?php
// P2-A: method first-class callable $obj->m(...) — bound closure, dispatches
// through op_init_dyn like a method call
class Calc {
    public int $base = 10;
    public function add($n) { return $this->base + $n; }
    public function getBase() { return $this->base; }
    public static function stat($n) { return $n * 2; }
}

$c = new Calc();
$f = $c->add(...);
var_dump($f(5));
var_dump($f(32));

$g = $c->getBase(...);
var_dump($g());

$s = $c->stat(...);
var_dump($s(21));

// chained: closure keeps receiver alive across mutations
$c->base = 100;
var_dump($f(1));

// passes through call_user_func
var_dump(call_user_func($c->add(...), 7));

// EXPECT: int(15)
// EXPECT: int(42)
// EXPECT: int(10)
// EXPECT: int(42)
// EXPECT: int(101)
// EXPECT: int(107)
