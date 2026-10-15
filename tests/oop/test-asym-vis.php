<?php
// EXPECT: read=1 3
// EXPECT: setx=9
// EXPECT: e1=Cannot modify private(set) property A::$x from global scope
// EXPECT: e2=Cannot modify private(set) property A::$y from scope Sub
// EXPECT: est=Cannot modify private(set) property S::$sx from global scope
// EXPECT: promo=4
// EXPECT: ep=Cannot modify private(set) property P::$q from global scope
// EXPECT: subpw=t5
// EXPECT: e8=Cannot modify protected(set) property AP::$t from global scope
// EXPECT: bool(true)
// EXPECT: eu=Cannot unset private(set) property A::$x from global scope
// EXPECT: ok
// P3: PHP 8.4 asymmetric visibility（private(set)/protected(set)，与 PHP 对拍）
class A {
    public private(set) int $x = 1;
    protected private(set) int $y = 2;
    private(set) int $z = 3;
    function setX(int $v) { $this->x = $v; }
}
$a = new A();
echo "read=", $a->x, " ", $a->z, "\n";
$a->setX(9);
echo "setx=", $a->x, "\n";
try { $a->x = 5; } catch (Error $e) { echo "e1=", $e->getMessage(), "\n"; }
class Sub extends A {
    function m() { $this->y = 7; echo "subw=y", $this->y, "\n"; }
}
try { (new Sub())->m(); } catch (Error $e) { echo "e2=", $e->getMessage(), "\n"; }
class S { public private(set) static int $sx = 1; }
try { S::$sx = 2; } catch (Error $e) { echo "est=", $e->getMessage(), "\n"; }
class P { function __construct(public private(set) int $q) {} }
$p = new P(4);
echo "promo=", $p->q, "\n";
try { $p->q = 5; } catch (Error $e) { echo "ep=", $e->getMessage(), "\n"; }
class AP { public protected(set) int $t = 1; }
class APS extends AP { function w() { $this->t = 5; echo "subpw=t", $this->t, "\n"; } }
(new APS())->w();
$ap = new AP();
try { $ap->t = 9; } catch (Error $e) { echo "e8=", $e->getMessage(), "\n"; }
var_dump(isset($a->x));
try { unset($a->x); } catch (Error $e) { echo "eu=", $e->getMessage(), "\n"; }
echo "ok\n";
