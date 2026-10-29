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
