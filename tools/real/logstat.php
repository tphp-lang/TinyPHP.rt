<?php
/* logstat.php - P0-7 真实负载样本：访问日志统计工具
 *
 * 真实工具而非微基准：解析常见 combined 格式日志，统计每 IP 请求数、
 * 总字节、错误率，输出摘要（真实脚本的典型形态：字符串切分 + 关联
 * 数组计数 + 排序 + 标量聚合，foreach 主循环——JIT 白名单外的循环
 * 形态正是"体感标尺"要暴露的）。
 *
 * CLI 直跑：php tphp tools/real/logstat.php   （输出统计摘要）
 * bench 复用：define('TPHP_BENCH', 1); require ...; 调用函数。
 */

/* 合成 N 行 combined 格式日志（确定性，供双端对拍） */
function logstat_make_log($n) {
    $ips = ["10.0.0.1", "10.0.0.2", "192.168.1.10", "172.16.0.9", "10.1.2.3"];
    $out = "";
    for ($i = 0; $i < $n; $i++) {
        $ip = $ips[$i % 5];
        $code = ($i % 17 === 0) ? "404" : "200";
        $out .= $ip . ' - - [28/Oct/2026:12:' . ($i % 60) . ':00 +0000] "GET /p'
              . ($i % 50) . ' HTTP/1.1" ' . $code . ' ' . (200 + ($i % 800)) . "\n";
    }
    return $out;
}

/* 核心统计：返回单行摘要（bench 用全等校验） */
function logstat($log) {
    $lines = explode("\n", $log);
    $counts = [];
    $bytes = 0;
    $errs = 0;
    foreach ($lines as $ln) {
        if ($ln === "") continue;
        $sp = explode(" ", $ln, 10);
        $ip = $sp[0];
        if (isset($counts[$ip])) {
            $counts[$ip] = $counts[$ip] + 1;
        } else {
            $counts[$ip] = 1;
        }
        $bytes += intval($sp[9]);
        $c = $sp[8];
        if ($c === "404" || $c === "500") $errs++;
    }
    /* 手写 top-1 遍历（真实惯用法；tphp 内置缺 arsort/asort/ksort——
     * compat 缺口记录在案，此处不依赖） */
    $topIp = "";
    $top = 0;
    foreach ($counts as $ip => $cnt) {
        if ($cnt > $top) { $top = $cnt; $topIp = $ip; }
    }
    return "lines=" . count($lines) . " ips=" . count($counts)
         . " bytes=" . $bytes . " errs=" . $errs
         . " top=" . $top . "/" . $topIp;
}

if (!defined("TPHP_BENCH")) {
    $log = logstat_make_log(20000);
    echo logstat($log) . "\n";
}
