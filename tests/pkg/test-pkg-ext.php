<?php
// EXPECT: src-ok
// EXPECT: badtype-rej-ok
// EXPECT: single-rej-ok
// EXPECT: add-ok
// EXPECT: writeback-ok
// EXPECT: nonproj-ok
// EXPECT: rownp=tst/extcmd|y|y|-
// EXPECT: sync-installed
// EXPECT: sync-present
// EXPECT: lock-ext-ok
// EXPECT: lock-ext-ic-ok
// EXPECT: ic-tamper-blocked
// EXPECT: force-update-ok
// EXPECT: readd-ok
// EXPECT: row1=tst/extcmd|y|y|y
// EXPECT: row2=tst/ghost|n|n|y
// EXPECT: sync-miss-rej-ok
// EXPECT: remove-ok
// EXPECT: remove-wb-ok
// EXPECT: cleanup-ok

// ext 命令组（PLAN/ext-packages.md §4 切片 4 + §5 编译链入）：
// 非法 type 拒 / 单段名拒 / 项目写回 / 非项目不写 /
// --list 三列 source×built-in×project（无项目列 "-"；声明缺源 n） /
// sync 补齐 + 幂等 / 缺源码报错 / remove 时 ext[] 写回 /
// add/sync 后自动 relink → built-in 变 y（切片 5）。
// 落点 <tphp_root>/ext = build/ext（tphp 根 = build/）。
$root = __DIR__ . "/../..";
$src = $root . "/build/pkgtest-ext-src";
$bad = $root . "/build/pkgtest-ext-bad";
$single = $root . "/build/pkgtest-ext-single";
$app = $root . "/build/pkgtest-ext-app";
$noproj = $root . "/build/pkgtest-ext-noproj";
$exttst = $root . "/build/ext/tst";

function ext_rm9($d) {
    // POSIX 上 scandir(文件)=ENOTDIR；先判类型再递归（同 pkg_rm_tree9）
    if (file_exists($d) && !is_dir($d)) { unlink($d); return; }
    if (!file_exists($d)) return;
    $entries = scandir($d);
    if ($entries === false) return;
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        ext_rm9($d . "/" . $f);
    }
    if (file_exists($d)) rmdir($d);
}

// 表行提取：固定列宽 %-20s %-6s %-8s %s → 竖线归一（按行首名字过滤）
function ext_row($tbl, $name, $pfx) {
    foreach (explode("\n", $tbl) as $ln) {
        if (strpos($ln, $name) !== 0) continue;
        return $pfx . rtrim(substr($ln, 0, 20)) . "|" . trim(substr($ln, 22, 6))
             . "|" . trim(substr($ln, 30, 8)) . "|" . trim(substr($ln, 40)) . "\n";
    }
    return $pfx . "MISSING\n";
}

foreach ([$src, $bad, $single, $app, $noproj, $exttst] as $d)
    if (file_exists($d)) ext_rm9($d);

// --- 源 1：合法 ext 包 tst/extcmd（§2 契约） ---
// 注意：tphp mkdir 忽略第 3 参 recursive（记档缺口），逐层创建
mkdir($src, 0777, true);
mkdir($src . "/src", 0777, true);
mkdir($src . "/include", 0777, true);
file_put_contents($src . "/mod.json", json_encode([
    "name" => "tst/extcmd",
    "version" => "0.1.0",
    "type" => "ext",
    "ext" => [
        "sources" => "src",
        "include" => ["include/"],
        "tphp_abi" => "0.0.2",
        "init" => "tphp_ext_tst_extcmd_init",
    ],
]));
file_put_contents($src . "/src/extcmd.c",
    "void tphp_ext_tst_extcmd_init(void) {}\n");
echo "src-ok\n";

// --- 源 2：type=app → 拒（§2.1 type 必须 ext）；源 3：单段名拒 ---
pkg_new($bad, "extbad");
mkdir($single, 0777, true);
file_put_contents($single . "/mod.json", json_encode([
    "name" => "singleseg",
    "version" => "0.1.0",
    "type" => "ext",
    "ext" => ["sources" => "src"],
]));

// 项目骨架 + 非项目空目录
pkg_new($app, "ext-app");
mkdir($noproj, 0777, true);

$e1 = "";
try { pkg_ext_add($app, $bad); }
catch (Throwable $e) { $e1 = $e->getMessage(); }
echo strpos($e1, "not an ext package") !== false
    ? "badtype-rej-ok\n" : "badtype-FAIL: " . $e1 . "\n";

$e2 = "";
try { pkg_ext_add($app, $single); }
catch (Throwable $e) { $e2 = $e->getMessage(); }
echo strpos($e2, "vendor/package") !== false
    ? "single-rej-ok\n" : "single-FAIL: " . $e2 . "\n";

// --- 项目内 add → 写回 ext[]（§4.1） ---
$addok = true;
try { pkg_ext_add($app, $src); }
catch (Throwable $e) { echo "add-ex: ", $e->getMessage(), "\n"; $addok = false; }
echo $addok ? "add-ok\n" : "add-FAIL\n";

$am = json_decode(file_get_contents($app . "/mod.json"), true);
// 逐级守卫（3 维 isset 中间键缺失会 raise = tphp 缺口，记档待拍板）
$ext = (is_array($am) && isset($am["ext"])) ? $am["ext"] : [];
$wb = count($ext) === 1;
if ($wb) {
    $e0 = $ext[0];
    $wb = isset($e0["name"]) && $e0["name"] === "tst/extcmd"
       && isset($e0["url"]) && $e0["url"] === $src;
}
echo $wb ? "writeback-ok\n" : "writeback-FAIL\n";

// --- 非项目 add：只装不写（无 mod.json 生成） ---
$nok = true;
try { pkg_ext_add($noproj, $src); }
catch (Throwable $e) { echo "nowb-ex: ", $e->getMessage(), "\n"; $nok = false; }
echo ($nok && !file_exists($noproj . "/mod.json")) ? "nonproj-ok\n" : "nonproj-FAIL\n";

// 非项目视角 list：project 列 "-"
echo ext_row(pkg_ext_list($noproj), "tst/extcmd", "rownp=");

// --- sync：删源后补齐 → 再跑幂等（§4.2） ---
ext_rm9($exttst);
$s1 = pkg_ext_sync($app);
echo trim($s1) === "tst/extcmd: installed"
    ? "sync-installed\n" : "sync1-FAIL: " . trim($s1) . "\n";

$s2 = pkg_ext_sync($app);
echo trim($s2) === "tst/extcmd: already present"
    ? "sync-present\n" : "sync2-FAIL: " . trim($s2) . "\n";

// --- lock ext 分栏 + integrity（§6 切片 6） ---
$lk = file_get_contents($app . "/mod.lock");
$has_ext = strpos($lk, '"ext"') !== false
        && strpos($lk, "tst/extcmd") !== false;
echo $has_ext ? "lock-ext-ok\n" : "lock-ext-FAIL\n";
// integrity 快照：含源文件、不含 build/ 产物（.o）
// 注意：url 本身可能含 /build/ 路径，故不查整串 "/build/"
$has_ic = strpos($lk, '"integrity"') !== false
       && strpos($lk, "src/extcmd.c") !== false
       && strpos($lk, ".o\"") === false;
echo $has_ic ? "lock-ext-ic-ok\n" : "lock-ext-ic-FAIL\n";

// 篡改已装 ext → 再 add（覆盖前 integrity）应 fail-closed
// 包路径 = build/ext/<vendor>/<pkg> = build/ext/tst/extcmd
file_put_contents($exttst . "/extcmd/src/extcmd.c",
    "void tphp_ext_tst_extcmd_init(void) { /* tampered */ }\n");
$ic_block = false; $ic_msg = "";
try { pkg_ext_add($app, $src); }
catch (Throwable $e) { $ic_block = true; $ic_msg = $e->getMessage(); }
echo ($ic_block && strpos($ic_msg, "integrity") !== false)
    ? "ic-tamper-blocked\n" : "ic-tamper-FAIL: " . $ic_msg . "\n";

// --update（force）跳过 integrity 重装恢复
$uok = true;
try { pkg_ext_update($app, "tst/extcmd"); }
catch (Throwable $e) { echo "force-upd-ex: ", $e->getMessage(), "\n"; $uok = false; }
echo $uok ? "force-update-ok\n" : "force-update-FAIL\n";

// 恢复后再 add 应通过（内容与 lock 一致）
$a2ok = true;
try { pkg_ext_add($app, $src); }
catch (Throwable $e) { echo "readd-ex: ", $e->getMessage(), "\n"; $a2ok = false; }
echo $a2ok ? "readd-ok\n" : "readd-FAIL\n";

// --- ghost 声明（缺源）+ list 三列 ---
$am = json_decode(file_get_contents($app . "/mod.json"), true);
$am["ext"][] = ["name" => "tst/ghost", "url" => "../no-such-ext", "ref" => ""];
file_put_contents($app . "/mod.json", json_encode($am));

$lst = pkg_ext_list($app);
echo ext_row($lst, "tst/extcmd", "row1=");
echo ext_row($lst, "tst/ghost", "row2=");

// --- sync 撞缺源码 → 报错带名字（§4.2 失败路径） ---
$e3 = "";
try { pkg_ext_sync($app); }
catch (Throwable $e) { $e3 = $e->getMessage(); }
echo strpos($e3, "tst/ghost") !== false
    ? "sync-miss-rej-ok\n" : "sync-miss-FAIL: " . $e3 . "\n";

// --- remove：删目录 + ext[] 写回移除 ---
$rok = true;
try { pkg_ext_remove($app, "tst/extcmd"); }
catch (Throwable $e) { echo "remove-ex: ", $e->getMessage(), "\n"; $rok = false; }
echo $rok ? "remove-ok\n" : "remove-FAIL\n";

$am = json_decode(file_get_contents($app . "/mod.json"), true);
$wb2 = isset($am["ext"]) && count($am["ext"]) === 1
    && $am["ext"][0]["name"] === "tst/ghost";
echo $wb2 ? "remove-wb-ok\n" : "remove-wb-FAIL\n";

foreach ([$src, $bad, $single, $app, $noproj, $exttst] as $d)
    if (file_exists($d)) ext_rm9($d);
echo "cleanup-ok\n";
