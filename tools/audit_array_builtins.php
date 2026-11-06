<?php
// 刀13：array 族病态审计（function_exists 守卫 = 缺口清单一并产出）
$n = 20000;
$a = []; for ($i = 0; $i < $n; $i++) $a[] = ($i * 7919) % 65536;
$b = []; for ($i = 0; $i < $n; $i++) $b[] = ($i * 104729) % 65536;
$assoc = []; for ($i = 0; $i < 5000; $i++) $assoc["k$i"] = $i;
$rows = []; for ($i = 0; $i < 20000; $i++) $rows[] = ["id" => $i, "g" => $i % 100];

function tf($name, $f) {
    $t0 = microtime(true);
    try { $f(); } catch (Throwable $e) { echo str_pad($name, 22), "MISSING\n"; return; }
    $t1 = microtime(true);
    echo str_pad($name, 22), round(($t1 - $t0) * 1000, 1), " ms\n";
}

tf("array_unique", function () use ($a) { return array_unique($a); });
tf("array_diff", function () use ($a, $b) { return array_diff($a, $b); });
tf("array_intersect", function () use ($a, $b) { return array_intersect($a, $b); });
tf("array_count_values", function () use ($a) { return array_count_values($a); });
tf("array_flip", function () use ($a) { return array_flip($a); });
tf("array_column", function () use ($rows) { return array_column($rows, "id"); });
tf("in_array", function () use ($a) { $c = 0; for ($i = 0; $i < 2000; $i++) { if (in_array($i * 31, $a)) $c++; } return $c; });
tf("array_search", function () use ($a) { $c = 0; for ($i = 0; $i < 2000; $i++) { if (array_search($i * 31, $a) !== false) $c++; } return $c; });
tf("array_keys", function () use ($assoc) { return array_keys($assoc); });
tf("array_values", function () use ($assoc) { return array_values($assoc); });
tf("array_merge", function () use ($a, $b) { return array_merge($a, $b); });
tf("array_reverse", function () use ($a) { return array_reverse($a); });
tf("array_slice", function () use ($a) { return array_slice($a, 100, 5000); });
tf("array_sum", function () use ($a) { return array_sum($a); });
tf("array_chunk", function () use ($a) { return array_chunk($a, 100); });
tf("array_combine", function () use ($a, $b) { return array_combine(array_slice($a, 0, 10000), array_slice($b, 0, 10000)); });
tf("array_splice", function () use ($a) { $c = $a; array_splice($c, 100, 10, [9, 9, 9]); return $c; });
tf("str_replace", function () use ($a) { return str_replace("1", "one", implode(",", $a)); });
tf("strncmp", function () { return strncmp("abcdef", "abcxyz", 3); });
tf("strcasecmp", function () { return strcasecmp("Abc", "abd"); });
tf("strncasecmp", function () { return strncasecmp("Abcdef", "ABCxyz", 3); });
tf("strtr", function () use ($a) { return strtr(implode(",", $a), ["1" => "one"]); });
tf("ucfirst", function () { return ucfirst("hello world"); });
tf("strrev", function () { return strrev("abcdef"); });
echo "audit-done\n";
