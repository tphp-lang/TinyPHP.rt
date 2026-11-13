<?php
// object recon r4 (2026-11-13): dtor-path decomposition + CI target echo + residual splits.
// Baseline context: recon3 post prop-v1.5/5B' showed full delta ~+6.4ms/1e5 dominated by
// the dtor path (alloc_ctor_dtor +5.81, dtor_via_unset +5.35, alloc_ctor +0.29, alloc_ctor_get +0.37).
// This probe splits that delta into: loop skeleton / anon-vs-named / empty-method frames /
// empty-dtor dispatch / observable dtor body / dead (DCE-able) dtor prop write / str value move.
function bench_one($name, $fn, $n = 5) {
    $times = array();
    for ($i = 0; $i < $n; $i++) {
        $t0 = microtime(true);
        $r = $fn();
        $t1 = microtime(true);
        $times[] = ($t1 - $t0) * 1000.0;
        if ($r === null) echo "NULL ", $name, "\n";
    }
    sort($times);
    echo $name, " min=", $times[0], " med=", $times[(int)($n/2)], "\n";
    return $times[0];
}

$N = 100000;
$sink = 0;
$GLOBALS['gdc'] = 0;

// --- 0. CI target echo: exact `object 1e5` bench shape (anon class + sink) ---
bench_one("bench_echo", function () use ($N) {
    $sink = 0;
    for ($i = 0; $i < $N; $i++) {
        $o = new class($i) {
            public $v;
            public function __construct($v) { $this->v = $v; }
            public function get() { return $this->v; }
            public function __destruct() { $this->v = -1; }
        };
        $sink = $o->get();
        unset($o);
    }
    return $sink;
});

// --- 0b. named twin of bench_echo (anon per-site snapshot check: 0 minus this = anon tax) ---
class Full {
    public $v;
    function __construct($v) { $this->v = $v; }
    function get() { return $this->v; }
    function __destruct() { $this->v = -1; }
}
bench_one("named_full", function () use ($N) {
    $sink = 0;
    for ($i = 0; $i < $N; $i++) {
        $o = new Full($i);
        $sink = $o->get();
        unset($o);
    }
    return $sink;
});

// --- 1. loop skeleton: loop + CV write only (JIT boundary / op execution floor) ---
bench_one("loop_skeleton", function () use ($N) {
    $sink = 0;
    for ($i = 0; $i < $N; $i++) { $sink = $i; }
    return $sink;
});

// --- 2. empty warm method call (frame push/destroy isolate; vs recon3 method_get_warm) ---
class Noop {
    public $v;
    function __construct($v) { $this->v = $v; }
    function n() { }
}
$noo = new Noop(7);
bench_one("mcall_empty", function () use ($N, $noo) {
    for ($i = 0; $i < $N; $i++) { $noo->n(); }
    return $noo->v;
});

// --- 3. empty dtor via unset (dispatch + free isolate, no body) ---
class EmptyD {
    public $v;
    function __construct($v) { $this->v = $v; }
    function __destruct() { }
}
bench_one("edtor_via_unset", function () use ($N) {
    $o = new EmptyD(0);
    for ($i = 0; $i < $N; $i++) {
        unset($o);
        $o = new EmptyD($i);
    }
    return $o->v;
});

// --- 4. dtor body with observable side effect (global counter): no DCE possible ---
class GlbD {
    public $v;
    function __construct($v) { $this->v = $v; }
    function __destruct() { $GLOBALS['gdc']++; }
}
bench_one("dtor_global_via_unset", function () use ($N) {
    $GLOBALS['gdc'] = 0;
    $o = new GlbD(0);
    for ($i = 0; $i < $N; $i++) {
        unset($o);
        $o = new GlbD($i);
    }
    return $GLOBALS['gdc'];
});

// --- 5. dtor body with prop write (dead store after destruction; PHP JIT may DCE it) ---
class Dt {
    public $v;
    function __construct($v) { $this->v = $v; }
    function __destruct() { $this->v = -1; }
}
bench_one("dtor_body_via_unset", function () use ($N) {
    $o = new Dt(0);
    for ($i = 0; $i < $N; $i++) {
        unset($o);
        $o = new Dt($i);
    }
    return $o->v;
});

// --- 6. warm prop write with refcounted value, alternating strings (anti-DCE) ---
class W2 {
    public $v;
    public $sv;
}
$w2 = new W2(0);
$s1 = "aaaa";
$s2 = "bbbb";
bench_one("prop_assign_str", function () use ($N, $w2, $s1, $s2) {
    for ($i = 0; $i < $N; $i++) { $w2->sv = ($i & 1) ? $s1 : $s2; }
    return strlen($w2->sv);
});

// --- 7. warm prop int write (guard + scalar move floor; vs recon3 prop_fetch_only) ---
$w3 = new W2(0);
bench_one("prop_assign_int", function () use ($N, $w3) {
    for ($i = 0; $i < $N; $i++) { $w3->v = $i; }
    return $w3->v;
});

echo "sink=", $sink, " gdc=", $GLOBALS['gdc'], " done\n";
