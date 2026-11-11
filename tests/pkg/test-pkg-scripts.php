<?php
// EXPECT: src-ok
// EXPECT: add-ok
// EXPECT: pkg-pre=install:pre:demo/ps
// EXPECT: pkg-post=install:post
// EXPECT: root-add=install:pre:1|install:post:1
// EXPECT: readd-ok
// EXPECT: root-readd=install:pre:1|install:post:1
// EXPECT: remove-ok
// EXPECT: root-remove=install:pre:1|install:post:1|remove:pre:1|remove:post:1
// EXPECT: ignore-ok
// EXPECT: cleanup-ok

// mod.json.scripts：根+包 pre/post install/remove（PLAN/pkg-manager.md §2.1）
$root = __DIR__ . "/../..";
$src = $root . "/build/pkgtest-scripts-src";
$app = $root . "/build/pkgtest-scripts-app";

function pkg_rm_tree9($d) {
    // POSIX 上 scandir(文件)=ENOTDIR warning 噪音（win FindFirstFile 静默）——
    // 先判类型（PHP 惯用法），非目录直接删
    if (file_exists($d) && !is_dir($d)) { unlink($d); return; }
    $entries = scandir($d);
    if ($entries === false) { if (file_exists($d)) unlink($d); return; }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree9($d . "/" . $f);
    }
    if (file_exists($d)) rmdir($d);
}

if (file_exists($src)) pkg_rm_tree9($src);
if (file_exists($app)) pkg_rm_tree9($app);

// --- 源库包：preInstall/postInstall ---
pkg_new($src, "demo/ps", true);
mkdir($src . "/scripts", 0777, true);
file_put_contents($src . "/scripts/pre.php",
    '<?php file_put_contents(__DIR__ . "/../pre.txt",'
    . ' getenv("TPHP_PKG_ACTION") . ":" . getenv("TPHP_PKG_PHASE")'
    . ' . ":" . getenv("TPHP_PKG_NAME"));');
file_put_contents($src . "/scripts/post.php",
    '<?php file_put_contents(__DIR__ . "/../post.txt",'
    . ' getenv("TPHP_PKG_ACTION") . ":" . getenv("TPHP_PKG_PHASE"));');
$m = json_decode(file_get_contents($src . "/mod.json"), true);
$m["scripts"] = [
    "preInstall" => "scripts/pre.php",
    "postInstall" => "scripts/post.php",
];
file_put_contents($src . "/mod.json", json_encode($m));
echo "src-ok\n";

// --- 根应用：root hooks ---
pkg_new($app, "ps-app");
mkdir($app . "/scripts", 0777, true);
file_put_contents($app . "/scripts/root.php",
    '<?php file_put_contents(__DIR__ . "/../root.txt",'
    . ' getenv("TPHP_PKG_ACTION") . ":" . getenv("TPHP_PKG_PHASE")'
    . ' . ":" . getenv("TPHP_PKG_ROOT") . "|", FILE_APPEND);');
$rm = json_decode(file_get_contents($app . "/mod.json"), true);
$rm["scripts"] = [
    "preInstall" => "scripts/root.php",
    "postInstall" => "scripts/root.php",
    "preRemove" => "scripts/root.php",
    "postRemove" => "scripts/root.php",
];
file_put_contents($app . "/mod.json", json_encode($rm));

// --- add ---
$add_ok = true;
try { pkg_add($app, $src); }
catch (Throwable $e) { echo "add-ex: ", $e->getMessage(), "\n"; $add_ok = false; }
if ($add_ok) echo "add-ok\n";
if (!$add_ok) {
    // CI-only fail 探针（11-11，stderr 不进 EXPECT；R: DIAG 直出）：
    // 定位 preInstall "script not found" 时 fixture 的实际存在态
    error_log("PROBE app=" . (file_exists($app) ? "y" : "n")
        . " scriptsdir=" . (file_exists($app . "/scripts") ? "y" : "n")
        . " rootphp=" . (file_exists($app . "/scripts/root.php") ? "y" : "n")
        . " src=" . (file_exists($src) ? "y" : "n")
        . " srcpre=" . (file_exists($src . "/scripts/pre.php") ? "y" : "n"));
}

$pre = @file_get_contents($app . "/vendor/demo/ps/pre.txt");
$post = @file_get_contents($app . "/vendor/demo/ps/post.txt");
// 失败路径防御（11-11）：fgc 失败=false → tphp trim(bool) 抛 TypeError（PHP 弱
// 模式 coerce ""，真语义差另记档）会把进程打死在行中、掩盖后续断言对比——
// 测试域先防御，让 add 失败时全部期望行照常输出
echo "pkg-pre=", is_string($pre) ? trim($pre) : "", "\n";
echo "pkg-post=", is_string($post) ? trim($post) : "", "\n";

$root1 = @file_get_contents($app . "/root.txt");
echo "root-add=", rtrim(trim(is_string($root1) ? $root1 : ""), "|"), "\n";

// --- 再 add（同源 = update 路径；根仍按 url 相同判 update）---
$re_ok = true;
try { pkg_add($app, $src); }
catch (Throwable $e) { echo "readd-ex: ", $e->getMessage(), "\n"; $re_ok = false; }
if ($re_ok) echo "readd-ok\n";
$root2 = @file_get_contents($app . "/root.txt");
echo "root-readd=", rtrim(trim(is_string($root2) ? $root2 : ""), "|"), "\n";

// --- remove ---
$rm_ok = true;
try { pkg_remove($app, "demo/ps"); }
catch (Throwable $e) { echo "remove-ex: ", $e->getMessage(), "\n"; $rm_ok = false; }
if ($rm_ok) echo "remove-ok\n";
$root3 = @file_get_contents($app . "/root.txt");
echo "root-remove=", rtrim(trim(is_string($root3) ? $root3 : ""), "|"), "\n";

// --- ignore scripts ---
// TPHP_PKG_SCRIPTS=0 在 C 侧 getenv 缓存前设置：用 putenv 后新开操作。
// 本进程内 scripts_enabled 每次读 getenv（非 static 缓存）→ putenv 即可。
putenv("TPHP_PKG_SCRIPTS=0");
@unlink($app . "/root.txt");
pkg_add($app, $src);
$ig = !file_exists($app . "/root.txt");
echo $ig ? "ignore-ok\n" : "ignore-FAIL\n";
putenv("TPHP_PKG_SCRIPTS"); // unset-ish

pkg_rm_tree9($src);
pkg_rm_tree9($app);
echo "cleanup-ok\n";
