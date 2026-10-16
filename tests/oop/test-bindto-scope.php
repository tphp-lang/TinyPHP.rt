<?php
// B3: bindTo scope 语义——"static" 保留原 scope、scope=声明类可读父类 private、
// private 仅声明类精确可见、static:: 回落 $this 类（PHP 对拍 ref15/q18）
// EXPECT: 1: Ahi
// EXPECT: 2: Aph
// EXPECT: 3: Error: Call to protected method A2::ph() from scope Closure
// EXPECT: 4: Aph
// EXPECT: 5: Error: Call to private method A2::hi() from scope Closure
// EXPECT: 6: trp
// EXPECT: 7: err: Call to private method P9::f() from scope C9
// EXPECT: p1: Apriv
// EXPECT: p3: Error: Cannot access protected property A::$q
class A2 { private function hi() { return "Ahi"; } protected function ph() { return "Aph"; } }
class B2 extends A2 {}
$c = function() { return $this->hi(); };
echo "1: ", $c->bindTo(new B2, "A2")(), "\n";
$c2 = function() { return $this->ph(); };
echo "2: ", $c2->bindTo(new B2, "A2")(), "\n";
try { $r3 = $c2->bindTo(new B2)(); echo "3: ", $r3, "\n"; } catch (Throwable $e) { echo "3: ", get_class($e), ": ", $e->getMessage(), "\n"; }
echo "4: ", $c2->bindTo(new B2, "B2")(), "\n";
try { $r5 = $c->bindTo(new B2)(); echo "5: ", $r5, "\n"; } catch (Throwable $e) { echo "5: ", get_class($e), ": ", $e->getMessage(), "\n"; }
trait TR { private function trp() { return "trp"; } public function calltr() { return $this->trp(); } }
class TU { use TR; }
echo "6: ", (new TU)->calltr(), "\n";
class P9 { private function f() { return "pf"; } }
class C9 extends P9 { function g1() { try { return parent::f(); } catch (Throwable $e) { return "err: " . $e->getMessage(); } } }
echo "7: ", (new C9)->g1(), "\n";

class A { private $p = "Apriv"; protected $q = "Aq"; }
class B extends A {}
$o = new B;
$cc = function() { return $this->p; };
echo "p1: ", $cc->bindTo($o, "A")(), "\n";
$cq = function() { return $this->q; };
try { $cq->bindTo(new A, "static")(); } catch (Throwable $e) { echo "p3: ", get_class($e), ": ", $e->getMessage(), "\n"; }
