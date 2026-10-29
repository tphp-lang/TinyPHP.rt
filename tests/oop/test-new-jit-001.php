<?php
// P0-14 批2（INIT_NEW 对象链）：new/method/unset 序回放进 JIT
// （vm.c tphp_jit_opseq）。raise 场景在 JIT 外 catch，实测 deopt 传播链。
class Np {
    public $v;
    function __construct($v) { $this->v = $v; }
    function get() { return $this->v; }
    function add($d) { return $this->v + $d; }
}
// EXPECT: 45
function new_loop($n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) {
        $o = new Np($i);
        $s += $o->get();
        unset($o);
    }
    return $s;
}
echo new_loop(10), "\n";
// EXPECT: 7
function new_args() {
    $p = new Np(3);
    $q = $p->add(4);
    unset($p);
    return $q;
}
echo new_args(), "\n";
// EXPECT: 84
function new_anon($x) {
    $o = new class($x) {
        public $v;
        function __construct($v) { $this->v = $v; }
        function get() { return $this->v; }
    };
    $r = $o->get() * 2;
    unset($o);
    return $r;
}
echo new_anon(42), "\n";
// EXPECT: Error:Class "NoSuchCls" not found
function new_undefined() { return new NoSuchCls(); }
try { new_undefined(); } catch (Error $e) { echo get_class($e), ":", $e->getMessage(), "\n"; }
// EXPECT: Exception:ctor boom
function new_ctor_raise() {
    $o = new class(1) {
        function __construct($v) { throw new Exception("ctor boom"); }
    };
    return $o;
}
try { new_ctor_raise(); } catch (Exception $e) { echo get_class($e), ":", $e->getMessage(), "\n"; }
// EXPECT: 3
function new_no_ctor() {
    $o = new class {};
    unset($o);
    return 3;
}
echo new_no_ctor(), "\n";

// ---- 批2-2：prop 单 op pad 执行（FETCH/ASSIGN/ISSET/UNSET_PROP）----
class Pd {
    public $v;
    public $w;   // tphp 语义：未声明属性无 __set 时赋值静默丢弃（非 PHP 动态创建）
    function __construct($v) { $this->v = $v; }
}
// EXPECT: 6|yes|gone
function prop_rw($n) {
    $o = new Pd($n);
    $o->w = $o->v + 3;
    $a = $o->w;
    $k = isset($o->w) ? "yes" : "no";
    unset($o->w);
    $g = isset($o->w) ? "y" : "gone";
    unset($o);
    return $a . "|" . $k . "|" . $g;
}
echo prop_rw(3), "\n";

class Mg {
    public $a = 1;
    function __get($n) { return "get:" . $n; }
    function __set($n, $v) { $this->a = 9; }
}
// EXPECT: get:zz|9
function magic_rw() {
    $o = new Mg();
    $x = $o->zz;      // 未声明 → __get（magic 走真 handler，回放内同步跑）
    $o->zz = "v";     // 未声明 → __set（把 a 置 9）
    $y = $o->a;
    unset($o);
    return $x . "|" . $y;
}
echo magic_rw(), "\n";
