<?php
// logstat 百菜专项：组件段复核（HEAD=a23870c，刀18 探针重建版）
// 每段 min-of-6；tphp vs php8.5.1-nts strongest 对拍
function seg($name, $fn) {
    $best = 1e9;
    for ($i = 0; $i < 6; $i++) {
        $t0 = microtime(true);
        $r = $fn();
        $ms = (microtime(true) - $t0) * 1000;
        if ($ms < $best) $best = $ms;
        if ($r === null) echo "";
    }
    error_log(sprintf("SEG %-16s %8.2f", $name, $best));
}

// 样本（段外构建）
$ips = ["10.0.0.1", "10.0.0.2", "192.168.1.10", "172.16.0.9", "10.1.2.3"];
$mkline = function ($i) use ($ips) {
    $ip = $ips[$i % 5];
    $code = ($i % 17 === 0) ? "404" : "200";
    return $ip . ' - - [28/Oct/2026:12:' . ($i % 60) . ':00 +0000] "GET /p'
         . ($i % 50) . ' HTTP/1.1" ' . $code . ' ' . (200 + ($i % 800)) . "\n";
};

// 1) make 全段（插值 concat 链 + MOD + 分配）
seg("make_full", function () use ($mkline) {
    $out = "";
    for ($i = 0; $i < 20000; $i++) $out .= $mkline($i);
    return strlen($out);
});

// 2) 插值 concat 链（无 MOD：固定模板拼接）
seg("concat_chain", function () use ($ips) {
    $out = "";
    for ($i = 0; $i < 20000; $i++) {
        $out .= $ips[$i & 3] . ' - - [28/Oct/2026:12:00:00 +0000] "GET /p'
              . ' HTTP/1.1" 200 ' . (200 + $i) . "\n";
    }
    return strlen($out);
});

// 3) MOD 循环（5 个取模/行）
seg("mod_loop", function () {
    $s = 0;
    for ($i = 0; $i < 20000; $i++) {
        $s += ($i % 5) + ($i % 17 === 0 ? 1 : 0) + ($i % 60) + ($i % 50) + ($i % 800);
    }
    return $s;
});

// 样本日志（段外构建）
$log = "";
for ($i = 0; $i < 20000; $i++) $log .= $mkline($i);

// 4) split_only：explode 行+字段（已知 0.96× 反超面，对照）
seg("split_only", function () use ($log) {
    $lines = explode("\n", $log);
    $n = 0;
    foreach ($lines as $ln) {
        if ($ln === "") continue;
        $sp = explode(" ", $ln, 10);
        $n += strlen($sp[0]);
    }
    return $n;
});

// 5) kv_count：isset+递增+写回（2e4 行）
seg("kv_count", function () use ($log) {
    $lines = explode("\n", $log);
    $counts = [];
    foreach ($lines as $ln) {
        if ($ln === "") continue;
        $ip = substr($ln, 0, 12);
        if (isset($counts[$ip])) {
            $counts[$ip] = $counts[$ip] + 1;
        } else {
            $counts[$ip] = 1;
        }
    }
    return count($counts);
});

// 6) strlen 调用循环（2e4）
seg("strlen_loop", function () use ($log) {
    $n = 0;
    for ($i = 0; $i < 20000; $i++) $n += strlen($log) & 7;
    return $n;
});

// 7) intval+cmp 循环（2e4）
seg("intval_loop", function () use ($log) {
    $e = 0;
    for ($i = 0; $i < 20000; $i++) {
        $v = intval("200");
        if ($v === 404 || $v === 500) $e++;
    }
    return $e;
});

echo "ok\n";
