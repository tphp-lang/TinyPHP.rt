<?php
// EXPECT: read=1
// EXPECT: write=5 5
// EXPECT: via_str=5
// EXPECT: show=10
// EXPECT: sub_redecl=9
// EXPECT: dim=9
// EXPECT: post=1 2
// EXPECT: pre=3
// EXPECT: assignop=x!
// EXPECT: sub_arr=7
// EXPECT: isset_decl=true
// EXPECT: isset_missing=false
// EXPECT: isset_uninit=false
// EXPECT: empty_missing=true
// EXPECT: empty_set=false
// EXPECT: empty_uninit=true
// EXPECT: err_vis=Cannot access protected property A::$p
// EXPECT: err_base=Class name must be a valid object or a string
// EXPECT: ok
// P2-B: $obj::$x 实例/类名串访问静态属性（与 PHP 8.5 对拍）
class A {
    public static $x = 1;
    public static $arr = [1, 2];
    public static $n = 1;
    public static $s = "x";
    protected static $p = 2;
    static function bump($o) { $o::$x = 10; }
}
class B extends A {
    public static $arr = [7];
    public static $x = 9;
}
class C extends A {}
class U { public static int $t; }

$a = new A();
$b = new B();
$c = new C();
$u = new U();

echo "read=", $a::$x, "\n";
$a::$x = 5;
echo "write=", A::$x, " ", $c::$x, "\n";
$cls = "A";
echo "via_str=", $cls::$x, "\n";
A::bump($a);
echo "show=", $a::$x, "\n";
echo "sub_redecl=", $b::$x, "\n";
$a::$arr[0] = 9;
echo "dim=", A::$arr[0], "\n";
echo "post=", $a::$n++, " ", A::$n, "\n";
++$a::$n;
echo "pre=", $a::$n, "\n";
$a::$s .= "!";
echo "assignop=", $a::$s, "\n";
echo "sub_arr=", $b::$arr[0], "\n";
echo "isset_decl=", isset($a::$x) ? "true" : "false", "\n";
echo "isset_missing=", isset($a::$nope) ? "true" : "false", "\n";
echo "isset_uninit=", isset($u::$t) ? "true" : "false", "\n";
echo "empty_missing=", empty($a::$nope2) ? "true" : "false", "\n";
echo "empty_set=", empty($a::$x) ? "true" : "false", "\n";
echo "empty_uninit=", empty($u::$t) ? "true" : "false", "\n";
try { $z = $a::$p; } catch (Error $e) { echo "err_vis=", $e->getMessage(), "\n"; }
try { $i = 1; $j = $i::$x; } catch (Error $e) { echo "err_base=", $e->getMessage(), "\n"; }
echo "ok\n";
