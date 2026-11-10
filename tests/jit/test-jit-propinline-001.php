<?php
// prop v1.5 batch1: FETCH_PROP IR inline fast path (PLAN/prop-ic.md 2.1)
// Four-mode: default / TPHP_PROP_INLINE=0 / TPHP_JIT=0 / PHP 8.5
// EXPECT lines must match stdout exactly across all modes.

echo "--- propinline ---\n";

class PiBase {
    public $v = 11;
    public function get() { return $this->v; }
}
class PiChild extends PiBase {
    public $v = 22;
    public function get() { return $this->v; }
}
class PiPriv {
    private $p = 33;
    protected $q = 44;
    public function gp() { return $this->p; }
    public function gq() { return $this->q; }
    public function sp($x) { $this->p = $x; return $this->p; }
}
class PiHook {
    public $h = 55;
    public function __get($n) { return "G:$n"; }
    public function __set($n, $v) { $this->h = $v; }
}
class PiTyped {
    public int $t = 66;
    public function gt() { return $this->t; }
    public function st($x) { $this->t = $x; return $this->t; }
}
class PiRo {
    public readonly int $r;
    public function __construct() { $this->r = 77; }
    public function gr() { return $this->r; }
}

// s1: public read/write main path
$a1 = new PiBase();
$b1 = $a1->get();
$a1->v = 100;
$c1 = $a1->v;
echo "s1=$b1,$c1\n";

// s2: child redeclare (different slot, name-ptr check)
$a2 = new PiChild();
$b2 = $a2->get();
$a2->v = 200;
$c2 = $a2->v;
echo "s2=$b2,$c2\n";

// s3: private/protected (always slow path)
$a3 = new PiPriv();
$b3 = $a3->gp();
$c3 = $a3->gq();
$d3 = $a3->sp(88);
echo "s3=$b3,$c3,$d3\n";

// s4: magic get/set (declared prop reads fast; undeclared hits magic)
$a4 = new PiHook();
$b4 = $a4->h;
$a4->zz = 1;
$c4 = $a4->zz;
echo "s4=$b4,$c4\n";

// s5: typed prop (assign always slow; read can hit fast)
$a5 = new PiTyped();
$b5 = $a5->gt();
$c5 = $a5->st(99);
$d5 = $a5->t;
echo "s5=$b5,$c5,$d5\n";

// s6: readonly (assign always slow)
$a6 = new PiRo();
$b6 = $a6->gr();
echo "s6=$b6\n";

// s7: non-object raise
$s7 = 0;
try { $x = null; $x->v; } catch (Error $e) { $s7 = 1; }
echo "s7=$s7\n";

// s8: rc balance (STR prop 1e5 read/write)
$a8 = new PiBase();
$a8->v = str_repeat("x", 8);
$sum8 = 0;
for ($i = 0; $i < 100000; $i++) {
    $a8->v = str_repeat("y", 8);
    $sum8 += strlen($a8->v);
}
echo "s8=$sum8\n";

// s9: polymorphic rotation (parent/child alternate -> ic refill)
$p9 = new PiBase();
$c9 = new PiChild();
$sum9 = 0;
for ($i = 0; $i < 10000; $i++) {
    $o = ($i & 1) ? $p9 : $c9;
    $sum9 += $o->v;
}
echo "s9=$sum9\n";

// s10: private slow + public fast alternating (ic cross-site)
$a10 = new PiPriv();
$b10 = new PiBase();
$sum10 = 0;
for ($i = 0; $i < 10000; $i++) {
    $sum10 += $a10->gp();
    $sum10 += $b10->v;
}
echo "s10=$sum10\n";

// s11: assignment-as-expression dst ($x = ($o->v = 42))
$a11 = new PiBase();
$x11 = ($a11->v = 42);
$y11 = $a11->v;
echo "s11=$x11,$y11\n";

// s12: readonly re-assign raises (fast path must miss)
$s12 = 0;
$a12 = new PiRo();
try { $a12->r = 1; } catch (Error $e) { $s12 = 1; }
echo "s12=$s12\n";

// s13: typed wrong-type assign raises (fast path must miss)
$s13 = 0;
$a13 = new PiTyped();
try { $a13->t = "nope"; } catch (Error $e) { $s13 = 1; }
echo "s13=$s13\n";

// s14: hooks __set on undeclared (fast path miss -> magic)
$a14 = new PiHook();
$a14->zz = 7;
$y14 = $a14->zz;
echo "s14=$y14\n";

// s15: assign loop STR rc (expression form + write-back)
$a15 = new PiBase();
$sum15 = 0;
for ($i = 0; $i < 100000; $i++) {
    $r15 = ($a15->v = str_repeat("z", 8));
    $sum15 += strlen($r15);
}
echo "s15=$sum15\n";

echo "END\n";

// EXPECT: --- propinline ---
// EXPECT: s1=11,100
// EXPECT: s2=22,200
// EXPECT: s3=33,44,88
// EXPECT: s4=55,G:zz
// EXPECT: s5=66,99,99
// EXPECT: s6=77
// EXPECT: s7=1
// EXPECT: s8=800000
// EXPECT: s9=165000
// EXPECT: s10=440000
// EXPECT: s11=42,42
// EXPECT: s12=1
// EXPECT: s13=1
// EXPECT: s14=G:zz
// EXPECT: s15=800000
// EXPECT: END
