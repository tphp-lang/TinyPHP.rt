<?php
// EXPECT: src-ok
// EXPECT: add-ok
// EXPECT: obj-ok
// EXPECT: remove-ok
// EXPECT: cleanup-ok

// ext libs/ldflags 的 $PKG 前缀（PLAN/ext-packages.md §2.2）：scripts 产出
// 二进制中间产物落包内 build/，ldflags 以 "$PKG/build/xxx.o" 引用进本次链接。
// 链锁口径同 test-pkg-ext-scripts：stub.c 引用 extra.o 的符号——$PKG 展开
// 失效 → 链接 unresolved → add 抛错，断言即刻暴露。
$root = __DIR__ . "/../..";
$src = $root . "/build/pkgtest-pkgpath-src";
$app = $root . "/build/pkgtest-pkgpath-app";
$extdir = $root . "/build/ext/tst";            // 安装根（tphp root = build/）

function pp_rm9($d) {
    // POSIX 上 scandir(文件)=ENOTDIR warning 噪音——先判类型再递归
    if (file_exists($d) && !is_dir($d)) { unlink($d); return; }
    if (!file_exists($d)) return;
    $entries = scandir($d);
    if ($entries === false) return;
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pp_rm9($d . "/" . $f);
    }
    if (file_exists($d)) rmdir($d);
}

foreach ([$src, $app, $extdir] as $d)
    if (file_exists($d)) pp_rm9($d);

// --- 项目骨架（ext --add 落盘需要项目上下文）---
pkg_new($app, "pp-app");

// --- ext 包：ldflags 带 $PKG + scripts 产出 extra.o ---
// mod.json 用字面量固定内容（不依赖 json_encode）：本测试聚焦 $PKG 链锁，
// 字面量更直观；json_encode 嵌套键隔离回归另见 test-json-nested-keys.php。
mkdir($src, 0777, true);
mkdir($src . "/src", 0777, true);
mkdir($src . "/scripts", 0777, true);
file_put_contents($src . "/mod.json", '{' .
    '"name":"tst/pp","version":"0.1.0","type":"ext",' .
    '"ext":{' .
        '"sources":"src",' .
        '"ldflags":["$PKG/build/extra.o"],' .
        '"os":{"windows":{"cc":"clang"},"linux":{"cc":"cc"},"darwin":{"cc":"cc"}},' .
        '"tphp_abi":"0.0.2",' .
        '"init":"tphp_ext_tst_pp_init"' .
    '},' .
    '"scripts":{"postInstall":"scripts/geno.php","postUpdate":"scripts/geno.php"}' .
'}');
// stub 引用 extra.o 的符号：$PKG 展开失效 → 链接 unresolved → add 抛错
file_put_contents($src . "/src/stub.c",
    "extern void pkgpath_probe_sym(void);\n"
  . "void tphp_ext_tst_pp_init(void) { pkgpath_probe_sym(); }\n");
// codegen：cwd = dest（post 时机）；产出二进制 .o（非 .c）。
// 源文件落 build/（sources 扫描排除 build/）——否则 extra.c 会被流水线
// 再编译一份，与 $PKG 链入的 extra.o 重复定义符号。
// shell_exec：捕获 stdout（stderr 透传控制台）；成功判定 = extra.o 存在。
file_put_contents($src . "/scripts/geno.php", '<?php
@mkdir(__DIR__ . "/../build", 0777, true);
$file = __DIR__ . "/../build/extra_src.c";
file_put_contents($file, "void pkgpath_probe_sym(void) {}\n");
$obj = __DIR__ . "/../build/extra.o";
if (file_exists($obj)) unlink($obj);
$done = false;
foreach (["cc", "clang", "gcc"] as $cc) {
    shell_exec($cc . " -c -o " . $obj . " " . $file);
    if (file_exists($obj)) { $done = true; break; }
}
if (!$done) exit(1);
');
echo "src-ok\n";

// --- add（post 产出 .o + $PKG 展开链入）---
$addok = true;
try { pkg_ext_add($app, $src); }
catch (Throwable $e) { echo "add-ex: ", $e->getMessage(), "\n"; $addok = false; }
echo $addok ? "add-ok\n" : "add-FAIL\n";
echo (file_exists($extdir . "/pp/build/extra.o")) ? "obj-ok\n" : "obj-FAIL\n";

// --- remove ---
$rok = true;
try { pkg_ext_remove($app, "tst/pp"); }
catch (Throwable $e) { echo "rm-ex: ", $e->getMessage(), "\n"; $rok = false; }
echo ($rok && !file_exists($extdir . "/pp")) ? "remove-ok\n" : "remove-FAIL\n";

foreach ([$src, $app, $extdir] as $d)
    if (file_exists($d)) pp_rm9($d);
echo "cleanup-ok\n";
