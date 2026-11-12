<?php
// EXPECT: files-ok
// EXPECT: mod=tst/scaffold ext 0.1.0 MIT
// EXPECT: sym=tphp_ext_tst_scaffold_init
// EXPECT: notinstall-ok
// EXPECT: refuse-ok
// EXPECT: cleanup-ok

// pkg ext scaffold test（cwd 形态，2026-11-12 拍板）：
// new --ext 创建的是 ext **包** → 落调用方给定目录（CLI = cwd/<name>），
// 不得落 <tphp_root>/ext/（那是 ext --add/sync 的安装区）。
// 与 test-pkg-new-lib 同式：显式 dir 走 pkg_new_ext builtin，build/ 做 scratch。
$root = __DIR__ . "/../..";
$dir = $root . "/build/pkgtest-ext-scaffold";
$install_area = $root . "/build/ext/tst/scaffold"; // 旧行为的落点（安装区）

function pxs_rm($d) {
    if (file_exists($d) && !is_dir($d)) { unlink($d); return; }
    if (!file_exists($d)) return;
    $entries = scandir($d);
    if ($entries === false) return;
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pxs_rm($d . "/" . $f);
    }
    if (file_exists($d)) rmdir($d);
}

if (file_exists($dir)) pxs_rm($dir);
if (file_exists($install_area)) pxs_rm($install_area);

pkg_new_ext($dir, "tst/scaffold", false);

$need = ["mod.json", "src/scaffold.c", "include/scaffold.h",
         "README.md", ".gitignore", "examples/usage.php"];
$allok = true;
foreach ($need as $f) {
    if (!file_exists($dir . "/" . $f)) {
        echo "missing=", $f, "\n";
        $allok = false;
    }
}
echo $allok ? "files-ok\n" : "files-FAIL\n";

$m = json_decode(file_get_contents($dir . "/mod.json"), true);
echo "mod=", $m["name"], " ", $m["type"], " ", $m["version"], " ", $m["license"], "\n";

$init = (isset($m["ext"]) && isset($m["ext"]["init"])) ? $m["ext"]["init"] : "?";
echo "sym=", $init, "\n";

// 拍板锁定：不得出现在安装区 <tphp_root>/ext/
echo file_exists($install_area) ? "install-FAIL\n" : "notinstall-ok\n";

// 已存在 → 拒（无 --force）
$e1 = "";
try { pkg_new_ext($dir, "tst/scaffold", false); }
catch (Throwable $e) { $e1 = $e->getMessage(); }
echo strpos($e1, "already exists") !== false
    ? "refuse-ok\n" : "refuse-FAIL: " . $e1 . "\n";

pxs_rm($dir);
echo "cleanup-ok\n";
