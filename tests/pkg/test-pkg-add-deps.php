<?php
// EXPECT: setup-ok
// EXPECT: add-ok
// EXPECT: both-installed
// EXPECT: autoload-ok
// EXPECT: remove-ok
// EXPECT: orphan-cleaned
// EXPECT: cleanup-ok

// pkg 传递依赖：A 依赖 B；remove A 后 B 被清理
$root = __DIR__ . "/../..";
$bdir = $root . "/build/pkgtest-dep-b";
$adir = $root . "/build/pkgtest-dep-a";
$app = $root . "/build/pkgtest-app-dep";

function pkg_rm_tree3($d) {
    $entries = scandir($d);
    if ($entries === false) { unlink($d); return; }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree3($d . "/" . $f);
    }
    rmdir($d);
}

foreach ([$bdir, $adir, $app] as $d) if (file_exists($d)) pkg_rm_tree3($d);

// B：无依赖的库
pkg_new($bdir, "dep/b", true);

// A：依赖 B（url 相对 app 项目根）
pkg_new($adir, "dep/a", true);
$ma = json_decode(file_get_contents($adir . "/mod.json"));
$ma["deps"] = ["dep/b" => ["url" => "../pkgtest-dep-b", "ref" => ""]];
file_put_contents($adir . "/mod.json", json_encode($ma));
echo "setup-ok\n";

// 应用 add A → 递归装 B
pkg_new($app, "demo-app");
pkg_add($app, $adir);
echo "add-ok\n";

$vb = $app . "/vendor/dep/b/mod.json";
$va = $app . "/vendor/dep/a/mod.json";
echo (file_exists($va) && file_exists($vb)) ? "both-installed\n" : "install-FAIL\n";

$al = file_get_contents($app . "/vendor/autoload.php");
$has_a = strpos($al, "/dep/a/") !== false || strpos($al, "\\dep\\a\\") !== false
      || strpos($al, "dep/a/src") !== false;
$has_b = strpos($al, "/dep/b/") !== false || strpos($al, "\\dep\\b\\") !== false
      || strpos($al, "dep/b/src") !== false;
echo ($has_a && $has_b) ? "autoload-ok\n" : "autoload-FAIL\n";

// remove A → B 成为孤儿 → 一并清理
pkg_remove($app, "dep/a");
echo "remove-ok\n";

$va_gone = !file_exists($va);
$vb_gone = !file_exists($vb);
echo ($va_gone && $vb_gone) ? "orphan-cleaned\n" : "orphan-FAIL\n";

pkg_rm_tree3($bdir);
pkg_rm_tree3($adir);
pkg_rm_tree3($app);
echo "cleanup-ok\n";
