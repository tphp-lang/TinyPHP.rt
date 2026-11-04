<?php
// 刀2 综合探针：JIT 快线族正确性（JIT on vs TPHP_JIT=0）
// ① dst 释放缺失（泄漏）→ dtor 计数 <300；② CAST 非标量错值
class D {
    static $n = 0;
    function __destruct() { D::$n++; }
}
function fresh() { return new D(); }

// ①a ADD 快线裸 store 覆盖 refcounted dst
function t_add($a, $b) { $x = fresh(); $x = $a + $b; return $x; }
for ($i = 0; $i < 300; $i++) $r = t_add(1, 2);
echo "add=", D::$n, "/300\n"; D::$n = 0;

// ①b CMP 快线
function t_cmp($a, $b) { $x = fresh(); $x = ($a == $b); return $x; }
for ($i = 0; $i < 300; $i++) $r = t_cmp(1, 2);
echo "cmp=", D::$n, "/300\n"; D::$n = 0;

// ①c BOOL_NOT（无慢路）
function t_not($a) { $x = fresh(); $x = !$a; return $x; }
for ($i = 0; $i < 300; $i++) $r = t_not($i);
echo "not=", D::$n, "/300\n"; D::$n = 0;

// ①d CAST to bool（make_truthy → store_bool）
function t_cb($a) { $x = fresh(); $x = (bool)$a; return $x; }
for ($i = 0; $i < 300; $i++) $r = t_cb($i);
echo "castbool=", D::$n, "/300\n"; D::$n = 0;

// ①e OP_CONST 标量覆盖
function t_const() { $x = fresh(); $x = 42; return $x; }
for ($i = 0; $i < 300; $i++) $r = t_const();
echo "const=", D::$n, "/300\n"; D::$n = 0;

// ①f INC suffix dst($y 旧对象) != src($i)
function t_inc($i) { $y = fresh(); $y = $i++; return $y; }
for ($i = 0; $i < 300; $i++) $r = t_inc($i);
echo "incsuf=", D::$n, "/300\n"; D::$n = 0;

// ①g 调用结果覆盖 refcounted dst（direct call）
function id2($v) { return $v; }
function t_call() { $x = fresh(); $x = id2(7); return $x; }
for ($i = 0; $i < 300; $i++) $r = t_call();
echo "call=", D::$n, "/300\n"; D::$n = 0;

// ② CAST 非标量源错值
function ci() { $s = "5"; return (int)$s; }
for ($i = 0; $i < 300; $i++) $r = ci();
echo "cast_str_int=$r (want 5)\n";
function cf() { $s = "5.5"; return (float)$s; }
for ($i = 0; $i < 300; $i++) $r = cf();
echo "cast_str_float=$r (want 5.5)\n";
function cb2() { $s = "5"; return (bool)$s; }
for ($i = 0; $i < 300; $i++) $r = cb2();
echo "cast_str_bool=", $r ? "1" : "0", " (want 1)\n";
echo "END\n";
