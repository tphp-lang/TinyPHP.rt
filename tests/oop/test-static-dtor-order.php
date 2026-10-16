<?php
// B4: 收尾析构序——静态属性按类声明序释放（持有链级联触发 __destruct），
// 与 PHP shutdown 序对拍（q4d/q4e）
// EXPECT: end
// EXPECT: dtor:static-held
// EXPECT: dtor:cycle
// EXPECT: d1
// EXPECT: d2
class DD { public $tag; function __destruct() { echo "dtor:", $this->tag, "\n"; } }
class Hh { public static $o; public static $arr = []; }
Hh::$o = new DD; Hh::$o->tag = "static-held";
$a = new DD; $a->tag = "cycle"; Hh::$arr = [$a]; $a = null;
echo "end\n";

class D1 { function __destruct() { echo "d1\n"; } }
class D2 { function __destruct() { echo "d2\n"; } }
class A1x { public static $x; }
class B1x { public static $y; }
A1x::$x = new D1;
B1x::$y = new D2;
