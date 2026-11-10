<?php
// object recon round2: isolate prop vs method vs dtor vs alloc with JIT-form shapes
// CI-style: named class, no anon (anon parse cost may differ)

function bench_one($name, $fn, $n = 5) {
    $times = array();
    for ($i = 0; $i < $n; $i++) {
        $t0 = microtime(true);
        $fn();
        $t1 = microtime(true);
        $times[] = ($t1 - $t0) * 1000.0;
    }
    sort($times);
    $line = $name . " min=" . $times[0] . " med=" . $times[(int)($n/2)] . " all=";
    foreach ($times as $t) $line = $line . " " . $t;
    echo $line, "\n";
}

class P1 { public $v; }
class P2 {
    public $v;
    function __construct($v) { $this->v = $v; }
}
class P3 {
    public $v;
    function __construct($v) { $this->v = $v; }
    function get() { return $this->v; }
}
class P4 {
    public $v;
    function __construct($v) { $this->v = $v; }
    function get() { return $this->v; }
    function __destruct() { $this->v = -1; }
}

$N = 100000;
$w = new P3(1);

// A. direct prop r/w on warm object (no method, no alloc)
bench_one("A_prop_direct", function () use ($N, $w) {
    for ($i = 0; $i < $N; $i++) { $w->v = $i; $s = $w->v; }
});

// B. method get on warm object (no alloc)
bench_one("B_method_warm", function () use ($N, $w) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $s += $w->get(); }
});

// C. alloc empty (P1 no ctor/dtor)
bench_one("C_alloc_empty", function () use ($N) {
    for ($i = 0; $i < $N; $i++) { $o = new P1($i); $o->v = $i; }
});

// D. alloc + ctor (no dtor, no get)
bench_one("D_alloc_ctor", function () use ($N) {
    for ($i = 0; $i < $N; $i++) { $o = new P2($i); }
});

// E. alloc + ctor + dtor (no get)
bench_one("E_alloc_ctor_dtor", function () use ($N) {
    for ($i = 0; $i < $N; $i++) { $o = new P4($i); }
});

// F. alloc + ctor + get (dtor at scope)
bench_one("F_alloc_ctor_get", function () use ($N) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $o = new P3($i); $s += $o->get(); }
});

// G. full named (alloc+ctor+get+unset+dtor)
bench_one("G_full_named", function () use ($N) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $o = new P4($i);
        $s += $o->get();
        unset($o);
    }
});

// H. prop write in loop on fresh objects (no method)
bench_one("H_fresh_prop_write", function () use ($N) {
    for ($i = 0; $i < $N; $i++) { $o = new P1(0); $o->v = $i; }
});

echo "done\n";
