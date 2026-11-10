<?php
// object component survey (mirror kv recon methodology)
// Segments isolate: full / new+ctor / get / dtor / prop / named-class / method-only
// Usage: tphp run tools/probe_object_recon.php  OR  php tools/probe_object_recon.php

function bench_one($name, $fn, $n = 5) {
    $times = [];
    for ($i = 0; $i < $n; $i++) {
        $t0 = microtime(true);
        $fn();
        $t1 = microtime(true);
        $times[] = ($t1 - $t0) * 1000.0;
    }
    sort($times);
    $line = str_pad($name, 28) . " min=" . number_format($times[0], 2)
          . "  med=" . number_format($times[(int)($n/2)], 2) . "  all=";
    foreach ($times as $t) $line .= " " . number_format($t, 2);
    echo $line, "\n";
}

class RecNamed {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function get() { return $this->v; }
    public function __destruct() { $this->v = -1; }
}

class RecEmpty {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function __destruct() {}
}

class RecNoDtor {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function get() { return $this->v; }
}

// pre-created object for method-only (no alloc/dtor in loop)
$warm = new RecNamed(1);

$N = 100000;

// 0. full CI form (anon class, ctor+get+unset+dtor)
bench_one("full_anon", function () use ($N) {
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
});

// 1. named class same shape
bench_one("full_named", function () use ($N) {
    for ($i = 0; $i < $N; $i++) {
        $o = new RecNamed($i);
        $sink = $o->get();
        unset($o);
    }
});

// 2. new+ctor only (dtor still runs at scope end)
bench_one("new_ctor_only", function () use ($N) {
    for ($i = 0; $i < $N; $i++) {
        $o = new RecNamed($i);
    }
});

// 3. new+ctor no dtor body
bench_one("new_nodtor", function () use ($N) {
    for ($i = 0; $i < $N; $i++) {
        $o = new RecNoDtor($i);
    }
});

// 4. new+ctor+get no explicit unset (dtor at iter end)
bench_one("new_get_nounset", function () use ($N) {
    for ($i = 0; $i < $N; $i++) {
        $o = new RecNamed($i);
        $sink = $o->get();
    }
});

// 5. method call only on warm object
bench_one("method_only", function () use ($N, $warm) {
    $s = 0;
    for ($i = 0; $i < $N; $i++) {
        $s += $warm->get();
    }
    if ($s !== $N) echo "method-FAIL\n";
});

// 6. prop write+read only (no method)
bench_one("prop_rw_only", function () use ($N, $warm) {
    for ($i = 0; $i < $N; $i++) {
        $warm->v = $i;
        $s = $warm->v;
    }
});

// 7. ctor prop write only (empty dtor, no get)
bench_one("ctor_prop_emptydtor", function () use ($N) {
    for ($i = 0; $i < $N; $i++) {
        $o = new RecEmpty($i);
    }
});

// 8. unset-only on rotating objects (dtor cost isolated)
bench_one("unset_dtor_only", function () use ($N) {
    $objs = [];
    for ($i = 0; $i < 1000; $i++) $objs[] = new RecNamed($i);
    for ($i = 0; $i < $N; $i++) {
        unset($objs[$i % 1000]);
        $objs[$i % 1000] = new RecNamed($i);
    }
});

echo "done\n";
