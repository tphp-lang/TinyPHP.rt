<?php
// P0-18①: use-captured closures enter the JIT (definition point
// SEND_CAP/CLOSURE_INIT + closure-body eligibility + op_do_fcall direct-entry
// poff). TPHP_JIT=0/1 must produce identical output.
// EXPECT: t1=42
// EXPECT: t2=303
// EXPECT: t3=1:99
// EXPECT: t4=null-ish
// EXPECT: t5=20
// EXPECT: t6=42
// EXPECT: t7=15
// EXPECT: t8=42
// EXPECT: t9=10500

// 1) basic value capture, called from top level (op_do_fcall direct entry)
function cap_mk(int $a) {
    return function(int $b) use ($a) { return $a + $b; };
}
$f = cap_mk(40);
echo "t1=", $f(2), "\n";

// 2) two captured vars + loop inside closure body (JIT'd body reads capture
//    slots [0..n_cap); params land at slots[n_cap..) via direct-entry poff)
function cap_mk2(int $x) {
    $y = 100;
    return function(int $b) use ($x, $y) {
        $s = 0;
        for ($i = 0; $i < $b; $i++) { $s += $x + $y; }
        return $s;
    };
}
$g = cap_mk2(1);
echo "t2=", $g(3), "\n";

// 3) by-value isolation: mutation inside closure must not leak out
function cap_iso() {
    $n = 1;
    $f = function() use ($n) { $n = 99; return $n; };
    $r = $f();
    echo "t3=", $n, ":", $r, "\n";
}
cap_iso();

// 4) capturing an undefined variable -> null (raw read semantics)
function cap_undef() {
    return function() use ($zz) { return $zz ?? "null-ish"; };
}
$h = cap_undef();
echo "t4=", $h(), "\n";

// 5) closure creation inside a JIT'd function loop (caps stack push/pop
//    pairing through SEND_CAP/CLOSURE_INIT)
function cap_loop(int $k) {
    $acc = 0;
    for ($i = 0; $i < $k; $i++) {
        $f = function() use ($i) { return $i * 2; };
        $acc += $f();
    }
    return $acc;
}
echo "t5=", cap_loop(5), "\n";

// 6) dynamic call $f() (JIT'd caller -> call_dyn; captured closures take the
//    tphp_vm_call_callable fallback -- semantics must stay identical)
function cap_dyn(int $m) {
    return function() use ($m) { return $m * 7; };
}
$w = cap_dyn(6);
echo "t6=", $w(), "\n";

// 7) implicit $this capture inside a method (SEND_CAP source = CV 0, T_OBJ)
class CapHost {
    private int $v = 5;
    public function mk(): Closure {
        return function() { return $this->v * 3; };
    }
}
$c = new CapHost();
$m = $c->mk();
echo "t7=", $m(), "\n";

// 8) captured value used by a nested closure (capture of a closure)
function cap_nested(int $seed) {
    $outer = function(int $n) use ($seed) { return $seed + $n; };
    $inner = function() use ($outer) { return $outer(10) * 2; };
    return $inner;
}
$nc = cap_nested(11);
echo "t8=", $nc(), "\n";

// 9) hot loop calling a captured closure repeatedly (direct-entry stability)
function cap_hot(int $n) {
    $base = 3;
    $f = function(int $k) use ($base) { return $k * $base; };
    $acc = 0;
    for ($i = 0; $i < $n; $i++) { $acc += $f($i & 7); }
    return $acc;
}
echo "t9=", cap_hot(1000), "\n";
