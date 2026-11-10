<?php
// object recon r3: kill DCE with sinks; isolate ASSIGN_PROP vs FETCH_PROP vs dtor dispatch
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

class W { public $v; }
class Ctor {
    public $v;
    function __construct($v) { $this->v = $v; }
}
class Get {
    public $v;
    function __construct($v) { $this->v = $v; }
    function get() { return $this->v; }
}
class Dt {
    public $v;
    function __construct($v) { $this->v = $v; }
    function __destruct() { $this->v = -1; }
}
class Full {
    public $v;
    function __construct($v) { $this->v = $v; }
    function get() { return $this->v; }
    function __destruct() { $this->v = -1; }
}
class EmptyD {
    public $v;
    function __construct($v) { $this->v = $v; }
    function __destruct() { }
}

$N = 100000;
$w = new W(0);

// sink guards
$sink = 0;

// 1. direct ASSIGN_PROP + FETCH_PROP on warm object (prevents DCE via return)
bench_one("prop_direct_rw", function () use ($N, $w) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $w->v = $i; $s += $w->v; }
    return $s;
});

// 2. ASSIGN_PROP only
bench_one("prop_assign_only", function () use ($N, $w) {
    for ($i = 0; $i < $N; $i++) { $w->v = $i; }
    return $w->v;
});

// 3. FETCH_PROP only
bench_one("prop_fetch_only", function () use ($N, $w) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $s += $w->v; }
    return $s;
});

// 4. method get only (warm)
bench_one("method_get_warm", function () use ($N, $w) {
    $g = new Get(1);
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $s += $g->get(); }
    return $s;
});

// 5. alloc + ctor (prop write inside ctor)
bench_one("alloc_ctor", function () use ($N) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $o = new Ctor($i); $s += $o->v; }
    return $s;
});

// 6. alloc + ctor + empty dtor
bench_one("alloc_ctor_edtor", function () use ($N) {
    for ($i = 0; $i < $N; $i++) { $o = new EmptyD($i); }
    return $o === null ? 1 : 0;
});

// 7. alloc + ctor + dtor with prop write
bench_one("alloc_ctor_dtor", function () use ($N) {
    for ($i = 0; $i < $N; $i++) { $o = new Dt($i); }
    return 1;
});

// 8. alloc + ctor + get (no dtor method)
bench_one("alloc_ctor_get", function () use ($N) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $o = new Get($i); $s += $o->get(); }
    return $s;
});

// 9. full: alloc+ctor+get+unset+dtor
bench_one("full", function () use ($N) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $o = new Full($i);
        $s += $o->get();
        unset($o);
    }
    return $s;
});

// 10. dtor dispatch only: pre-alloc then unset+realloc (isolates dtor+free vs alloc)
bench_one("dtor_via_unset", function () use ($N) {
    $o = new Dt(0);
    for ($i = 0; $i < $N; $i++) {
        unset($o);
        $o = new Dt($i);
    }
    return 1;
});

// 11. alloc only (empty class, no ctor/dtor)
class Bare { public $v; }
bench_one("alloc_bare", function () use ($N) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) { $o = new Bare(); $o->v = $i; $s += $o->v; }
    return $s;
});

echo "sink=", $sink, " done\n";
