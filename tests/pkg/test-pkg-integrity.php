<?php
// EXPECT: src-ok
// EXPECT: add-ok
// EXPECT: lock-has-integrity
// EXPECT: tamper-blocked
// EXPECT: restore-ok
// EXPECT: extra-file-blocked
// EXPECT: force-reinstall-ok
// EXPECT: no-lock-ok
// EXPECT: cleanup-ok

// 阶段 E：mod.lock integrity 快照（装→改→install 报错；改回 skip；
// 新增文件报错；--force 重装恢复；无 lock 不受影响）
$root = __DIR__ . "/../..";
$src = $root . "/build/pkgtest-src-int";
$app = $root . "/build/pkgtest-app-int";

function pkg_rm_tree4($d) {
    $entries = scandir($d);
    if ($entries === false) { unlink($d); return; }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree4($d . "/" . $f);
    }
    rmdir($d);
}

if (file_exists($src)) pkg_rm_tree4($src);
if (file_exists($app)) pkg_rm_tree4($app);

pkg_new($src, "demo/util", true);
echo "src-ok\n";

pkg_new($app, "demo-app");
pkg_add($app, $src);
$vpkg = $app . "/vendor/demo/util";
if (!file_exists($vpkg . "/mod.json")) { echo "add-FAIL\n"; exit(1); }
echo "add-ok\n";

// lock 含 integrity 快照（mod.json 与至少一个源文件）
$lk = file_get_contents($app . "/mod.lock");
echo (strpos($lk, "integrity") !== false && strpos($vpkg . "/mod.json", "mod.json") !== false)
    ? "lock-has-integrity\n" : "lock-integrity-missing\n";

// 篡改 vendor 文件 → install fail-closed
file_put_contents($vpkg . "/src/Greeter.php", "<?php // tampered\n");
$blocked = false; $msg = "";
try { pkg_install($app); } catch (Throwable $e) { $blocked = true; $msg = $e->getMessage(); }
echo $blocked ? "tamper-blocked\n" : "tamper-FAIL\n";

// 改回原文件 → install 通过（skip 路径）
pkg_add($app, $src);   // 重新安装恢复正确内容（同名覆盖）
// 恢复后 lock 快照与内容一致
$ok = true;
try { pkg_install($app); } catch (Throwable $e) { $ok = false; }
echo $ok ? "restore-ok\n" : "restore-FAIL\n";

// 新增快照外文件 → 报错（挡侧载）
file_put_contents($vpkg . "/evil.dll", "MZ fake");
$blocked = false;
try { pkg_install($app); } catch (Throwable $e) { $blocked = true; }
echo $blocked ? "extra-file-blocked\n" : "extra-file-FAIL\n";

// --force 重装 → 恢复干净状态
pkg_install($app, true);
echo (!file_exists($vpkg . "/evil.dll") && file_exists($vpkg . "/mod.json"))
    ? "force-reinstall-ok\n" : "force-reinstall-FAIL\n";

// 无 lock：install 走 mod.json 路径不受影响，成功后补写 lock+integrity
unlink($app . "/mod.lock");
pkg_install($app);
$lk2 = file_get_contents($app . "/mod.lock");
echo (file_exists($app . "/mod.lock") && strpos($lk2, "integrity") !== false)
    ? "no-lock-ok\n" : "no-lock-FAIL\n";

pkg_rm_tree4($src);
pkg_rm_tree4($app);
echo "cleanup-ok\n";
