<?php
// 对象 ==/=== 语义 + spl_autoload_register + class_exists
// EXPECT: ===same: yes
// EXPECT: ===diff: no
// EXPECT: ==eq: yes
// EXPECT: ==neq: no
// EXPECT: ==cross-class: no
// EXPECT: ==scalar: no
// EXPECT: clone-==-yes: yes
// EXPECT: clone-===-no: no
// EXPECT: autoload-closure: loaded
// EXPECT: autoload-fnstr: loaded2
// EXPECT: exists-after: yes

// --- ===: identity ---
class Box {
    public $v;
    public function __construct($v) { $this->v = $v; }
}
$a = new Box(1);
$b = $a;
$c = new Box(1);
echo "===same: ", $a === $b ? "yes" : "no", "\n";
echo "===diff: ", $a === $c ? "yes" : "no", "\n";

// --- ==: same class + recursive property equality ---
$d = new Box(1);
echo "==eq: ", $a == $d ? "yes" : "no", "\n";
$e = new Box(2);
echo "==neq: ", $a == $e ? "yes" : "no", "\n";

class Sub extends Box {}
$f = new Sub(1);
echo "==cross-class: ", $a == $f ? "yes" : "no", "\n";
echo "==scalar: ", $a == 1 ? "yes" : "no", "\n";

$g = clone $a;
echo "clone-==-yes: ", $g == $a ? "yes" : "no", "\n";
echo "clone-===-no: ", $g === $a ? "yes" : "no", "\n";

// --- autoload ---
spl_autoload_register(function ($name) {
    echo "autoload-closure: loaded\n";
});
class_exists("Ghost1");

function ghost2_loader($name) {
    echo "autoload-fnstr: loaded2\n";
}
spl_autoload_register("ghost2_loader");
class_exists("Ghost2");

class Real {}
echo "exists-after: ", class_exists("Real") ? "yes" : "no", "\n";
