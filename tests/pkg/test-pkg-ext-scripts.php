<?php
// EXPECT: src-ok
// EXPECT: add-ok
// EXPECT: codegen-ok
// EXPECT: readd-ok
// EXPECT: readd-seq-ok
// EXPECT: update-ok
// EXPECT: remove-ok
// EXPECT: hook-seq-ok
// EXPECT: postrm-none-ok
// EXPECT: pre-continue-ok
// EXPECT: bpost-ok
// EXPECT: ignore-ok
// EXPECT: postfail-throw-ok
// EXPECT: postfail-noroll-ok
// EXPECT: cleanup-ok

// ext mod.json.scripts（PLAN/ext-packages.md §7，2026-11-16 翻案）：
// 六键解析（postRemove 不触发）/ pre 失败=警告+继续（镜像 lib/app 实证）/
// post=落盘后编译前（codegen 链锁：post 生成 gen.c 参与本次链接）/
// post 失败=抛错 + 不回滚 / TPHP_PKG_SCRIPTS=0 全跳过。
$root = __DIR__ . "/../..";
$srcA = $root . "/build/pkgtest-exsc-src";     // A：六键 + codegen 链锁
$app  = $root . "/build/pkgtest-exsc-app";
$exttst = $root . "/build/ext/tst";            // 安装根（tphp root = build/）
$srcB = $root . "/build/pkgtest-exsc-pref";    // B：pre 失败包
$srcC = $root . "/build/pkgtest-exsc-postf";   // C：post 失败包

function exsc_rm9($d) {
    // POSIX 上 scandir(文件)=ENOTDIR；先判类型再递归（同 ext_rm9）
    if (file_exists($d) && !is_dir($d)) { unlink($d); return; }
    if (!file_exists($d)) return;
    $entries = scandir($d);
    if ($entries === false) return;
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        exsc_rm9($d . "/" . $f);
    }
    if (file_exists($d)) rmdir($d);
}

foreach ([$srcA, $srcB, $srcC, $app, $exttst] as $d)
    if (file_exists($d)) exsc_rm9($d);

// 项目骨架（钩子 env TPHP_PROJECT_DIR / update 面依赖 ext[] 声明）
pkg_new($app, "ext-app");

// --- A 源：六键 + codegen 链锁 ---
// 链锁机制：stub.c 的 init 引用 gen 符号，gen.c **只由 postInstall 生成**——
// 若 post 落在编译后，本次链接 unresolved → add 抛错（add-ok 即时暴露）。
mkdir($srcA, 0777, true);
mkdir($srcA . "/src", 0777, true);
mkdir($srcA . "/scripts", 0777, true);
file_put_contents($srcA . "/mod.json", json_encode([
    "name" => "tst/scgen",
    "version" => "0.1.0",
    "type" => "ext",
    "ext" => [
        "sources" => "src",
        "tphp_abi" => "0.0.2",
        "init" => "tphp_ext_tst_scgen_init",
    ],
    "scripts" => [
        "preInstall" => "scripts/hook.php",
        "postInstall" => ["scripts/hook.php", "scripts/gen.php"],
        "preUpdate" => "scripts/hook.php",
        // update 路径 copy 会清掉上次生成的 gen.c → postUpdate 必须重新生成
        "postUpdate" => ["scripts/hook.php", "scripts/gen.php"],
        "preRemove" => "scripts/hook.php",
        "postRemove" => "scripts/postrm.php",
    ],
]));
file_put_contents($srcA . "/src/stub.c",
    "void tphp_ext_tst_scgen_gen(void);\n"
  . "void tphp_ext_tst_scgen_init(void) { tphp_ext_tst_scgen_gen(); }\n");
// 钩子写项目目录（TPHP_PROJECT_DIR）——标记不落包目录，避免被覆盖安装清掉
file_put_contents($srcA . "/scripts/hook.php",
    '<?php file_put_contents(getenv("TPHP_PROJECT_DIR") . "/hook.txt",'
  . ' getenv("TPHP_PKG_ACTION") . ":" . getenv("TPHP_PKG_PHASE") . "|", FILE_APPEND);');
// codegen：cwd = dest（post 时机 = 落盘后编译前）
file_put_contents($srcA . "/scripts/gen.php",
    '<?php file_put_contents(__DIR__ . "/../src/gen.c",'
  . ' "void tphp_ext_tst_scgen_gen(void) {}\n");');
file_put_contents($srcA . "/scripts/postrm.php",
    '<?php file_put_contents(getenv("TPHP_PROJECT_DIR") . "/postrm.txt", "x");');
echo "src-ok\n";

// --- A：add（六键解析放行 + post=编译前链锁） ---
$addok = true;
try { pkg_ext_add($app, $srcA); }
catch (Throwable $e) { echo "add-ex: ", $e->getMessage(), "\n"; $addok = false; }
echo $addok ? "add-ok\n" : "add-FAIL\n";
echo ($addok && file_exists($exttst . "/scgen/src/gen.c"))
    ? "codegen-ok\n" : "codegen-FAIL\n";

// --- A：再 add（dest 已在 → update 钩子） ---
$r1 = true;
try { pkg_ext_add($app, $srcA); }
catch (Throwable $e) { echo "readd-ex: ", $e->getMessage(), "\n"; $r1 = false; }
echo $r1 ? "readd-ok\n" : "readd-FAIL\n";
$seq2 = @file_get_contents($app . "/hook.txt");
echo (is_string($seq2) && $seq2 === "install:pre|install:post|update:pre|update:post|")
    ? "readd-seq-ok\n"
    : "readd-seq-FAIL: " . (is_string($seq2) ? $seq2 : "") . "\n";

// --- A：--update（force 重装 → update 钩子） ---
$uok = true;
try { pkg_ext_update($app, "tst/scgen"); }
catch (Throwable $e) { echo "upd-ex: ", $e->getMessage(), "\n"; $uok = false; }
echo $uok ? "update-ok\n" : "update-FAIL\n";

// --- A：remove（preRemove 触发 + 目录删除） ---
$rok = true;
try { pkg_ext_remove($app, "tst/scgen"); }
catch (Throwable $e) { echo "rm-ex: ", $e->getMessage(), "\n"; $rok = false; }
echo ($rok && !file_exists($exttst . "/scgen")) ? "remove-ok\n" : "remove-FAIL\n";

// 全序列：add×2 + readd×2 + update×2 + remove×1；postRemove 不在列（§7②）
$seq = @file_get_contents($app . "/hook.txt");
$want = "install:pre|install:post|update:pre|update:post|"
      . "update:pre|update:post|remove:pre|";
echo (is_string($seq) && $seq === $want)
    ? "hook-seq-ok\n"
    : "hook-seq-FAIL: " . (is_string($seq) ? $seq : "") . "\n";
echo !file_exists($app . "/postrm.txt") ? "postrm-none-ok\n" : "postrm-none-FAIL\n";

// --- B：pre 失败（exit 1）= 警告 + 继续（§7③ 镜像 lib/app 实证） ---
mkdir($srcB, 0777, true);
mkdir($srcB . "/src", 0777, true);
mkdir($srcB . "/scripts", 0777, true);
file_put_contents($srcB . "/mod.json", json_encode([
    "name" => "tst/pref",
    "version" => "0.1.0",
    "type" => "ext",
    "ext" => [
        "sources" => "src",
        "tphp_abi" => "0.0.2",
        "init" => "tphp_ext_tst_pref_init",
    ],
    "scripts" => [
        "preInstall" => "scripts/bpre.php",
        "postInstall" => "scripts/bpost.php",
    ],
]));
file_put_contents($srcB . "/src/pref.c",
    "void tphp_ext_tst_pref_init(void) {}\n");
file_put_contents($srcB . "/scripts/bpre.php",
    '<?php file_put_contents(getenv("TPHP_PROJECT_DIR") . "/bpre.txt", "x"); exit(1);');
file_put_contents($srcB . "/scripts/bpost.php",
    '<?php file_put_contents(getenv("TPHP_PROJECT_DIR") . "/bpost.txt", "x");');

$bok = true;
try { pkg_ext_add($app, $srcB); }
catch (Throwable $e) { echo "b-ex: ", $e->getMessage(), "\n"; $bok = false; }
echo ($bok && file_exists($app . "/bpre.txt"))
    ? "pre-continue-ok\n" : "pre-continue-FAIL\n";
echo file_exists($app . "/bpost.txt") ? "bpost-ok\n" : "bpost-FAIL\n";

// --- ignore-scripts：TPHP_PKG_SCRIPTS=0 全跳过 ---
@unlink($app . "/bpre.txt");
@unlink($app . "/bpost.txt");
putenv("TPHP_PKG_SCRIPTS=0");
$ig = true;
try { pkg_ext_add($app, $srcB); }
catch (Throwable $e) { echo "ig-ex: ", $e->getMessage(), "\n"; $ig = false; }
// 显式 =1 恢复：tphp putenv 无 = 形态在 ucrt 不删除（env 会卡在 0，实测）
putenv("TPHP_PKG_SCRIPTS=1");
echo ($ig && !file_exists($app . "/bpre.txt") && !file_exists($app . "/bpost.txt"))
    ? "ignore-ok\n" : "ignore-FAIL\n";

// --- C：post 失败（exit 1）= 抛错 + 不回滚（§7③） ---
mkdir($srcC, 0777, true);
mkdir($srcC . "/src", 0777, true);
mkdir($srcC . "/scripts", 0777, true);
file_put_contents($srcC . "/mod.json", json_encode([
    "name" => "tst/pc",
    "version" => "0.1.0",
    "type" => "ext",
    "ext" => [
        "sources" => "src",
        "tphp_abi" => "0.0.2",
        "init" => "tphp_ext_tst_pc_init",
    ],
    "scripts" => [
        "postInstall" => "scripts/cpost.php",
    ],
]));
file_put_contents($srcC . "/src/pc.c",
    "void tphp_ext_tst_pc_init(void) {}\n");
file_put_contents($srcC . "/scripts/cpost.php",
    '<?php file_put_contents(getenv("TPHP_PROJECT_DIR") . "/cpost.txt", "x"); exit(1);');

$cex = false;
$cmsg = "";
try { pkg_ext_add($app, $srcC); }
catch (Throwable $e) { $cex = true; $cmsg = $e->getMessage(); }
echo ($cex && strpos($cmsg, "post script failed") !== false)
    ? "postfail-throw-ok\n" : "postfail-throw-FAIL: " . $cmsg . "\n";

// 不回滚：目录已落盘 + ext[] 已写回（post 失败不撤状态）
$am = json_decode(file_get_contents($app . "/mod.json"), true);
$ext = (is_array($am) && isset($am["ext"])) ? $am["ext"] : [];
$wb = false;
foreach ($ext as $e0) {
    if (isset($e0["name"]) && $e0["name"] === "tst/pc") { $wb = true; break; }
}
echo ($wb && file_exists($exttst . "/pc/src/pc.c") && file_exists($app . "/cpost.txt"))
    ? "postfail-noroll-ok\n" : "postfail-noroll-FAIL\n";

foreach ([$srcA, $srcB, $srcC, $app, $exttst] as $d)
    if (file_exists($d)) exsc_rm9($d);
echo "cleanup-ok\n";
