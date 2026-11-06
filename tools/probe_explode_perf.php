<?php
// 刀7B 性能探针：镜像 logstat 的 big explode 成分（12MB 日志 → 2 万元素）。
// 同二进制 env 切换 A/B：默认（TPHP_EXACT_ALLOC 开）vs =0 关，min-of-6 交替。
$line = '192.168.1.1 - - [06/Nov/2026:00:00:00 +0800] "GET /path?x=1 HTTP/1.1" 200 1234 "-" "'
      . str_repeat("User-Agent-Padding-", 30) . '"';   // ~640B/行 × 20000 ≈ 12MB，对齐日志灾样
$log = "";
for ($i = 0; $i < 20000; $i++) $log .= $line . "\n";
$sum = 0;
$t0 = microtime(true);
for ($r = 0; $r < 4; $r++) {
    $parts = explode("\n", $log);
    $sum += count($parts);
}
$t1 = microtime(true);
echo "BENCH big-explode ", round(($t1 - $t0) * 1000, 1), " ms sum=$sum len=", strlen($log), "\n";
