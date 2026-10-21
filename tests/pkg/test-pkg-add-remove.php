<?php
// EXPECT: src-ok
// EXPECT: add-ok
// EXPECT: vendor-ok
// EXPECT: deps-ok
// EXPECT: autoload-ok
// EXPECT: lock-ok
// EXPECT: remove-ok
// EXPECT: cleaned-ok
// EXPECT: cleanup-ok

// pkg add/remove：本地目录源（无网络）
$root = __DIR__ . "/../..";
$src = $root . "/build/pkgtest-src-add";
$app = $root . "/build/pkgtest-app-add";

function pkg_rm_tree2($d) {
    $entries = scandir($d);
    if ($entries === false) { unlink($d); return; }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree2($d . "/" . $f);
    }
    rmdir($d);
}

if (file_exists($src)) pkg_rm_tree2($src);
if (file_exists($app)) pkg_rm_tree2($app);

// 源库包
pkg_new($src, "demo/util", true);
echo "src-ok\n";

// 目标应用
pkg_new($app, "demo-app");
$add_ok = true;
try {
    pkg_add($app, $src);
} catch (Throwable $e) {
    echo "add-ex: ", get_class($e), ": ", $e->getMessage(), "\n";
    $add_ok = false;
}
if ($add_ok) echo "add-ok\n";

// vendor 落盘
$vend = $app . "/vendor/demo/util/mod.json";
if (!file_exists($vend)) { echo "missing vendor\n"; exit(1); }
echo "vendor-ok\n";

// 根 deps 含 demo/util
$m = json_decode(file_get_contents($app . "/mod.json"));
$has = isset($m["deps"]["demo/util"]);
echo $has ? "deps-ok\n" : "deps-missing\n";

// autoload 含 PSR-4
$al = file_get_contents($app . "/vendor/autoload.php");
echo (strpos($al, "/demo/util/") !== false || strpos($al, "\\demo\\util\\") !== false)
    ? "autoload-ok\n" : "autoload-missing\n";

// lock 存在且含条目
if (file_exists($app . "/mod.lock")) {
    $lk = file_get_contents($app . "/mod.lock");
    echo (strpos($lk, "demo/util") !== false) ? "lock-ok\n" : "lock-empty\n";
} else {
    echo "lock-missing\n";
}

// remove
pkg_remove($app, "demo/util");
echo "remove-ok\n";

// vendor 与 deps 已清
$gone = !file_exists($vend);
$m2 = json_decode(file_get_contents($app . "/mod.json"));
$no_dep = !isset($m2["deps"]["demo/util"]);
echo ($gone && $no_dep) ? "cleaned-ok\n" : "clean-FAIL\n";

pkg_rm_tree2($src);
pkg_rm_tree2($app);
echo "cleanup-ok\n";
