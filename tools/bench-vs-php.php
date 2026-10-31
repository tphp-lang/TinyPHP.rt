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
    // 变量键：常量键会被 Zend optimizer 间歇性整段折叠（进程间双态 ~22ms/~1.7ms，已实测）
    $m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
          "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
    $keys = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
    $sum = 0;
    for ($r = 0; $r < 200000; $r++) {
        for ($k = 0; $k < 8; $k++) $sum += $m[$keys[$k]];
    }
    if ($sum !== 7200000) echo "kv-FAIL\n";
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

// P0-19 arm64-mac 排查探针：分配+GC 密集（str/obj 双分配），与计算无关。
// 五平台对照可分离"runner CPU 代差"vs"tphp 平台缺陷"：PHP 侧同机慢=CPU；
// PHP 正常而 tphp 慢=tphp 的 TLS/allocator/平台实现问题。
bench("alloc-probe 1e5", function () {
    for ($r = 0; $r < 100; $r++) {
        $a = [];
        for ($i = 0; $i < 1000; $i++) {
            $a[] = "s$i";
            $o = new class($i) { public $v; };
            $o->v = $i + 1;
        }
    }
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

// ---- P0-7 真实负载样本（真实工具，非微基准）----
define("TPHP_BENCH", 1);
require __DIR__ . "/real/logstat.php";

bench("real-logstat", function () {
    $log = logstat_make_log(20000);   // ~1.5MB combined 日志（生成在计时外的部分=样本装配）
    $r = logstat($log);
    if ($r !== "lines=20001 ips=5 bytes=11990000 errs=1177 top=4000/10.0.0.1")
        echo "logstat-FAIL\n";
});

bench("real-jsonagg", function () {
    $rows = [];
    for ($i = 0; $i < 2000; $i++)
        $rows[] = ["id" => $i, "cat" => $i % 7, "amt" => $i % 1000];
    $j = json_encode($rows);
    $sum = 0; $max = 0; $grp = [];
    for ($r = 0; $r < 5; $r++) {
        // 显式 assoc=true：PHP json_decode 默认返回 stdClass，tphp 无
        // stdClass 基建且默认 true（compat 缺口记录 backlog）——true 是
        // 双端一致的惯用写法
        $d = json_decode($j, true);
        foreach ($d as $row) {
            $sum += $row["amt"];
            $c = $row["cat"];
            if (isset($grp[$c])) $grp[$c] = $grp[$c] + 1; else $grp[$c] = 1;
            if ($row["amt"] > $max) $max = $row["amt"];
        }
    }
    // Σ(i%1000) i=0..1999 = 999000；5 轮 = 4995000；max = 999；
    // grp 横跨 5 轮累加：cat0 = 286×5 = 1430（双端独立一致印证）
    if ($sum !== 4995000 || $max !== 999 || $grp[0] !== 1430)
        echo "jsonagg-FAIL\n";
});

echo "all-ok\n";
