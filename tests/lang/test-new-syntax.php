<?php
// EXPECT: parenless=A
// EXPECT: spread=3 4
// EXPECT: objstatic=1
// EXPECT: self=Q A
// EXPECT: dyn=A
// EXPECT: dynobj=A
// EXPECT: bad=Class name must be a valid object or a string
// EXPECT: named=7
// new 表达式四缺口对拍 PHP 8.5：无括号 / spread / new self·parent·static / 动态类名

class A {
    public static $x = 1;
    public $v;
    function __construct($v = 0) { $this->v = $v; }
    function m() { return 7; }
}

// new ClassName without parentheses (PHP new_non_dereferenceable)
echo 'parenless=', get_class(new A), "\n";

// spread args on new — was a real bug: the spread array was sent as a single arg
class P {
    public $a, $b;
    function __construct($a, $b) { $this->a = $a; $this->b = $b; }
}
$p = new P(...[3, 4]);
echo 'spread=', $p->a, ' ', $p->b, "\n";

// $obj::$prop (P2-B)
echo 'objstatic=', (new A)::$x, "\n";

// new static / new parent inside a subclass
class Q extends A {
    function g() { return new static; }
    function h() { return new parent; }
}
$q = new Q;
echo 'self=', get_class($q->g()), ' ', get_class($q->h()), "\n";

// new $className (string) — documented as supported
$cn = 'A';
echo 'dyn=', get_class(new $cn), "\n";

// new $obj (object) uses the instance's class
$inst = new A;
echo 'dynobj=', get_class(new $inst), "\n";

// invalid class-name value
$n = 5;
try { new $n; } catch (Throwable $e) { echo 'bad=', $e->getMessage(), "\n"; }

// named ctor args still work alongside the INIT_NEW rec
$named = new A(v: 5);
echo 'named=', $named->v + 2, "\n";
