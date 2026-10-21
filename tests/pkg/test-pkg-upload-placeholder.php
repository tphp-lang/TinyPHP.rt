<?php
// EXPECT: empty-install-ok
// EXPECT: remove-missing-deny-ok
// EXPECT: cleanup-ok

// 阶段 D：upload 为 CLI 占位（stdout 指引由 CLI 手动验证）；
// 此处锁定 pkg 核心在 upload 相关命令序列下的行为。
$root = __DIR__ . "/../..";
$app = $root . "/build/pkgtest-upload";
function pkg_rm_tree5($d) {
    $entries = scandir($d);
    if ($entries === false) { unlink($d); return; }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree5($d . "/" . $f);
    }
    rmdir($d);
}
if (file_exists($app)) pkg_rm_tree5($app);

pkg_new($app, "demo-empty");
pkg_install($app);          // 无 deps：空 lock + autoload 重生成
echo "empty-install-ok\n";

$denied = false;
try {
    pkg_remove($app, "nope");
} catch (ValueError $e) {
    $denied = true;
}
echo $denied ? "remove-missing-deny-ok\n" : "remove-missing-FAIL\n";

pkg_rm_tree5($app);
echo "cleanup-ok\n";
