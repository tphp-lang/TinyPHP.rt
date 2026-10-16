<?php
// EXPECT: read=[]
// EXPECT: exists=false
// EXPECT: root=tphp://asset
// 未打包：ASSET_ROOT 读取返回 false（警告走 stderr，EXPECT 只比 stdout）
// T14: __DIR__ 相对读是主推写法（见 test-dir-fallback.php）；ASSET_ROOT 保留为
// 直接访问包根的次要途径（tphp://asset 前缀直接查 zip，不经磁盘）
$v = file_get_contents(ASSET_ROOT . "/nope.txt");
echo "read=[", $v === false ? "" : $v, "]\n";
echo "exists=", file_exists(ASSET_ROOT . "/nope.txt") ? "true" : "false", "\n";
echo "root=", ASSET_ROOT, "\n";
