<?php
// EXPECT: m1=1
// EXPECT: deep=1
// EXPECT: m2=0
// EXPECT: m3=1
// EXPECT: m4=0
// EXPECT: m5=0
// EXPECT: single=1
// EXPECT: cleanup-ok

// mkdir 第 3 参 recursive（PHP 语义；11-11 修复前被忽略 → 父目录缺失
// 时静默 false）。已存在目标 false、文件挡道 false、单层非 recursive
// 不回归。warning 走 stderr 不进 stdout 对拍。
$base = __DIR__ . "/../../build/mkdirrec-test";

function mr_rm($d) {
    if (file_exists($d) && !is_dir($d)) { unlink($d); return; }
    if (!file_exists($d)) return;
    $es = scandir($d);
    if ($es === false) return;
    foreach ($es as $f) {
        if ($f === "." || $f === "..") continue;
        mr_rm($d . "/" . $f);
    }
    if (file_exists($d)) rmdir($d);
}

if (file_exists($base)) mr_rm($base);

$r1 = mkdir($base . "/a/b/c", 0777, true);           // 父全缺 → 逐层建
echo "m1=", $r1 ? "1" : "0", "\n";
echo "deep=", is_dir($base . "/a/b/c") ? "1" : "0", "\n";

$r2 = @mkdir($base . "/a/b/c", 0777, true);          // 目标已存在 → false（@ 抑 warning）
echo "m2=", $r2 ? "1" : "0", "\n";

$r3 = mkdir($base . "/a/b/d/e", 0777, true);         // 部分已存在 → 建余下
echo "m3=", $r3 ? "1" : "0", "\n";

file_put_contents($base . "/f", "x");
$r4 = @mkdir($base . "/f/g", 0777, true);            // 中间层是文件 → false
echo "m4=", $r4 ? "1" : "0", "\n";

$r5 = @mkdir($base . "/a", 0777, true);              // 单层已存在 → false
echo "m5=", $r5 ? "1" : "0", "\n";

$r6 = mkdir($base . "/single");                      // 非 recursive 单层 → true
echo "single=", $r6 ? "1" : "0", "\n";

mr_rm($base);
echo "cleanup-ok\n";
