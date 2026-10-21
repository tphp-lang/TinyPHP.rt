<?php
// EXPECT: src-ok
// EXPECT: first-add-ok
// EXPECT: no-modjson-blocked
// EXPECT: git-blocked
// EXPECT: mismatch-blocked
// EXPECT: manual-delete-ok
// EXPECT: cleanup-ok

// 覆盖 vendor 前的安全检查：非包目录 / 含 .git / name 不符 一律拒绝
$root = __DIR__ . "/../..";
$src = $root . "/build/pkgtest-src-guard";
$app = $root . "/build/pkgtest-app-guard";

function pkg_rm_tree3($d) {
    $entries = scandir($d);
    if ($entries === false) { unlink($d); return; }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree3($d . "/" . $f);
    }
    rmdir($d);
}

if (file_exists($src)) pkg_rm_tree3($src);
if (file_exists($app)) pkg_rm_tree3($app);

pkg_new($src, "demo/util", true);
echo "src-ok\n";

pkg_new($app, "demo-app");
pkg_add($app, $src);
if (!file_exists($app . "/vendor/demo/util/mod.json")) { echo "first-add-FAIL\n"; exit(1); }
echo "first-add-ok\n";

$vd = $app . "/vendor/demo/util";

// 目标不是包目录（无 mod.json）→ 拒绝覆盖，用户文件保留
unlink($vd . "/mod.json");
file_put_contents($vd . "/mywork.txt", "user data");
$blocked = false;
try { pkg_add($app, $src); } catch (Throwable $e) { $blocked = true; }
echo ($blocked && file_exists($vd . "/mywork.txt")) ? "no-modjson-blocked\n" : "no-modjson-FAIL\n";

// 含 .git 工作区 → 拒绝
pkg_rm_tree3($vd);
pkg_add($app, $src);   // 恢复正常安装
mkdir($vd . "/.git");
file_put_contents($vd . "/.git/config", "[core]\n");
$blocked = false;
try { pkg_add($app, $src); } catch (Throwable $e) { $blocked = true; }
echo ($blocked && file_exists($vd . "/.git/config")) ? "git-blocked\n" : "git-FAIL\n";

// mod.json name 与目标不符 → 拒绝
unlink($vd . "/.git/config");
rmdir($vd . "/.git");
$mj = $vd . "/mod.json";
$m = json_decode(file_get_contents($mj), true);
$m["name"] = "demo/other";
file_put_contents($mj, json_encode($m));
$blocked = false;
try { pkg_add($app, $src); } catch (Throwable $e) { $blocked = true; }
echo ($blocked && file_exists($mj)) ? "mismatch-blocked\n" : "mismatch-FAIL\n";

// 手动删除后可正常重装（fail-closed，不 fail-forever）
pkg_rm_tree3($vd);
pkg_add($app, $src);
echo file_exists($vd . "/mod.json") ? "manual-delete-ok\n" : "manual-delete-FAIL\n";

pkg_rm_tree3($src);
pkg_rm_tree3($app);
echo "cleanup-ok\n";
