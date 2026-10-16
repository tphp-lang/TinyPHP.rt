<?php
// C1: 动态成员名 $obj->$v / $obj->$v() / $obj?->$v / 链式写回 / C::$$v / C::$v()
// 与 PHP 8.5 对拍（tests 同形探针 q1/q21）；不可见成员按“诊断即异常”固化。
// EXPECT: 1: pv
// EXPECT: 2: m:z
// EXPECT: 3: set
// EXPECT: 4: set-cat
// EXPECT: 5: 6
// EXPECT: 6: yes no
// EXPECT: 7: 42
// EXPECT: 8: set-cat
// EXPECT: 9: ns-ok
// EXPECT: 10: m:q
// EXPECT: 11: unset-ok
// EXPECT: 12: Error: Cannot access private property Q1::$sec
// EXPECT: 13: UndefinedError: Undefined property Q1::$8
// EXPECT: 14: TypeError: Value of type array cannot be converted to string
// EXPECT: 15: pv
// EXPECT: s1: sv
// EXPECT: s3: sv2
// EXPECT: s4: 11
// EXPECT: s5: 16
// EXPECT: s6: sv2
// EXPECT: s8: sm:q
// EXPECT: s10: yes no
// EXPECT: s11: 9
class Q1 {
    public $x = "pv";
    public $cnt = 5;
    private $sec = "priv";
    public function m($a) { return "m:$a"; }
}
$o = new Q1;
$prop = "x";
echo "1: ", $o->$prop, "\n";
$m = "m";
echo "2: ", $o->$m("z"), "\n";
$o->$prop = "set";
echo "3: ", $o->x, "\n";
$o->$prop .= "-cat";
echo "4: ", $o->x, "\n";
$c = "cnt";
$o->$c++;
echo "5: ", $o->cnt, "\n";
echo "6: ", isset($o->$prop) ? "yes" : "no", " ", isset($o->nope) ? "yes" : "no", "\n";
$o->x = ["k" => 1];
$o->$prop["k"] = 42;
echo "7: ", $o->x["k"], "\n";
$o->x = "set-cat";
$nl = "x";
echo "8: ", $o?->$nl, "\n";
$o2 = null;
echo "9: ", $o2?->$nl === null ? "ns-ok" : "ns-bad", "\n";
$fn = $o->$m(...);
echo "10: ", $fn("q"), "\n";
unset($o->x);
echo "11: ", isset($o->x) ? "kept" : "unset-ok", "\n";
$sec = "sec";
try { $r12 = $o->$sec; echo "12: ", $r12, "\n"; } catch (Throwable $e) { echo "12: ", get_class($e), ": ", $e->getMessage(), "\n"; }
$i = 8;
try { $o->$i; } catch (Throwable $e) { echo "13: ", get_class($e), ": ", $e->getMessage(), "\n"; }
$bad = [1];
try { $o->$bad; } catch (Throwable $e) { echo "14: ", get_class($e), ": ", $e->getMessage(), "\n"; }
function cap_probe($k) { $q = new Q1; return $q->$k; }
echo "15: ", cap_probe("x"), "\n";

class S1 { public static $sx = "sv"; public static $cnt = 10; public static function sm($a = "") { return "sm:$a"; } }
$k = "sx";
echo "s1: ", S1::$$k, "\n";
S1::$$k = "sv2";
echo "s3: ", S1::$sx, "\n";
$sc = "cnt";
S1::$$sc++;
echo "s4: ", S1::$cnt, "\n";
S1::$$sc += 5;
echo "s5: ", S1::$cnt, "\n";
$o3 = new S1;
echo "s6: ", $o3::$$k, "\n";
$sm = "sm";
echo "s8: ", S1::$sm("q"), "\n";
echo "s10: ", isset(S1::$$k) ? "yes" : "no", " ", isset(S1::$nope) ? "yes" : "no", "\n";
S1::$sx = ["z" => 1];
S1::$$k["z"] = 9;
echo "s11: ", S1::$sx["z"], "\n";
