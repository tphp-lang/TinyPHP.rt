<?php
// Cross-runtime micro-bench: tphp vs PHP 8.5.1 (pure PHP subset; no Tphp\*)
function bench($name, $fn) {
    $t0 = microtime(true);
    $fn();
    $ms = (microtime(true) - $t0) * 1000;
    error_log(sprintf("BENCH %-18s %8.1f ms", $name, $ms));
}

function fib28($n) { return $n < 2 ? $n : fib28($n - 1) + fib28($n - 2); }

bench("fib(28)", function () {
    $r = fib28(28);
    if ($r !== 317811) echo "fib-FAIL\n";
});

bench("call 1e6", function () {
    $noop = function ($x) { return $x; };
    $s = 0;
    for ($i = 0; $i < 1000000; $i++) $s = $noop($s);
});

bench("array-push 1e6", function () {
    $a = [];
    for ($i = 0; $i < 1000000; $i++) $a[] = $i;
});

bench("array-rmw 1e5", function () {
    $a = array_fill(0, 1000, 1);
    for ($r = 0; $r < 100; $r++) {
        for ($i = 0; $i < 1000; $i++) $a[$i] = $a[$i] + 1;
    }
});

bench("array-read 1e6", function () {
    $a = [];
    for ($i = 0; $i < 1000; $i++) $a[] = $i;
    $sum = 0;
    for ($r = 0; $r < 1000; $r++) {
        for ($i = 0; $i < 1000; $i++) $sum += $a[$i];
    }
    if ($sum !== 499500000) echo "read-FAIL\n";
});

bench("kv-read 1.6e6", function () {
    $m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
          "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
    $sum = 0;
    for ($r = 0; $r < 200000; $r++) {
        $sum += $m["alpha"] + $m["beta"] + $m["gamma"] + $m["delta"]
              + $m["epsilon"] + $m["zeta"] + $m["eta"] + $m["theta"];
    }
});

bench("string 4e4", function () {
    $base = "abcdefghij";
    $s = "";
    for ($i = 0; $i < 40000; $i++) $s .= $base;
    if (strlen($s) !== 400000) echo "str-FAIL\n";
});

bench("throw/catch 1e5", function () {
    $n = 0;
    for ($i = 0; $i < 100000; $i++) {
        try { throw new Exception("v"); } catch (Exception $e) { $n++; }
    }
});

bench("serialize 2e3", function () {
    $tree = [];
    for ($i = 0; $i < 50; $i++) $tree[] = ["id" => $i, "name" => "item$i", "tags" => ["a", "b", "c"]];
    for ($i = 0; $i < 2000; $i++) { $s = serialize($tree); $back = unserialize($s); }
});

bench("json 2e3", function () {
    $rows = [];
    for ($i = 0; $i < 100; $i++) $rows[] = ["id" => $i, "name" => "row$i", "ok" => true, "score" => 1.5];
    for ($i = 0; $i < 2000; $i++) { $j = json_encode($rows); $back = json_decode($j); }
});

bench("object 1e5", function () {
    // 匿名类在 PHP 8 支持；tphp 也支持（test-oop-anon）
    for ($i = 0; $i < 100000; $i++) {
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

echo "all-ok\n";
