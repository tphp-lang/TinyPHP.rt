<?php
// kv_count 归因拆解：isset / 读 / 写 三段独立 + 合计（2e4 行，5 个固定 IP 新串）
function seg($name, $fn) {
    $best = 1e9;
    for ($i = 0; $i < 6; $i++) {
        $t0 = microtime(true);
        $r = $fn();
        $ms = (microtime(true) - $t0) * 1000;
        if ($ms < $best) $best = $ms;
    }
    error_log(sprintf("SEG %-16s %8.2f", $name, $best));
}

// 行样本：每行新分配的 IP 串（镜像 explode $sp[0] 形态——内容同指针异）
$ips = ["10.0.0.1", "10.0.0.2", "192.168.1.10", "172.16.0.9", "10.1.2.3"];
$rows = [];
for ($i = 0; $i < 20000; $i++) {
    $rows[] = $ips[$i % 5] . " - - x";   // 新串含 IP 前缀
}

// 1) isset 读判定段
seg("isset_only", function () use ($rows) {
    $c = [];
    $n = 0;
    foreach ($rows as $ln) {
        $ip = substr($ln, 0, 12);
        if (isset($c[$ip])) $n++;
        else $c[$ip] = 1;    // 也要写（isset 语义必须写否则全 miss——改两段式)
    }
    return $n;
});

// 2) 预填后纯读段
$c2 = [];
foreach ($rows as $ln) { $c2[substr($ln, 0, 12)] = 1; }
seg("fetch_only", function () use ($rows, $c2) {
    $s = 0;
    foreach ($rows as $ln) {
        $ip = substr($ln, 0, 12);
        $s += $c2[$ip];
    }
    return $s;
});

// 3) 纯写段（覆盖写，键全已存在）
$c3 = [];
foreach ($rows as $ln) { $c3[substr($ln, 0, 12)] = 1; }
seg("assign_only", function () use ($rows, $c3) {
    foreach ($rows as $ln) {
        $ip = substr($ln, 0, 12);
        $c3[$ip] = 2;
    }
    return $c3["x"] ?? 0;
});

// 4) 真实形态（isset+读改写）对照
seg("full_form", function () use ($rows) {
    $counts = [];
    foreach ($rows as $ln) {
        $ip = substr($ln, 0, 12);
        if (isset($counts[$ip])) {
            $counts[$ip] = $counts[$ip] + 1;
        } else {
            $counts[$ip] = 1;
        }
    }
    return count($counts);
});

echo "ok\n";
