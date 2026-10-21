<?php
// EXPECT: setup-ok
// EXPECT: install-ok
// EXPECT: restored-ok
// EXPECT: autoload-ok
// EXPECT: offline-skip-ok
// EXPECT: cleanup-ok

// pkg install：按 mod.lock 恢复（离线跳过已装包）
$root = __DIR__ . "/../..";
$bdir = $root . "/build/pkgtest-inst-b";
$adir = $root . "/build/pkgtest-inst-a";
$app = $root . "/build/pkgtest-app-inst";

function pkg_rm_tree4($d) {
    $entries = scandir($d);
    if ($entries === false) { unlink($d); return; }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree4($d . "/" . $f);
    }
    rmdir($d);
}

foreach ([$bdir, $adir, $app] as $d) if (file_exists($d)) pkg_rm_tree4($d);

// B、A 两个本地库；A 依赖 B
pkg_new($bdir, "dep/b", true);
pkg_new($adir, "dep/a", true);
$ma = json_decode(file_get_contents($adir . "/mod.json"));
$ma["deps"] = ["dep/b" => ["url" => "../pkgtest-inst-b", "ref" => ""]];
file_put_contents($adir . "/mod.json", json_encode($ma));

pkg_new($app, "demo-app");
pkg_add($app, $adir);
echo "setup-ok\n";

// 删 vendor 模拟新机器；lock 仍在
pkg_rm_tree4($app . "/vendor");
pkg_install($app);
echo "install-ok\n";

$va = file_exists($app . "/vendor/dep/a/mod.json");
$vb = file_exists($app . "/vendor/dep/b/mod.json");
echo ($va && $vb) ? "restored-ok\n" : "restore-FAIL\n";

$al = file_get_contents($app . "/vendor/autoload.php");
$ha = strpos($al, "/dep/a/") !== false;
$hb = strpos($al, "/dep/b/") !== false;
echo ($ha && $hb) ? "autoload-ok\n" : "autoload-FAIL\n";

// 二次 install：全部已装 → 跳过，仍成功（离线友好）
pkg_install($app);
echo "offline-skip-ok\n";

pkg_rm_tree4($bdir);
pkg_rm_tree4($adir);
pkg_rm_tree4($app);
echo "cleanup-ok\n";
