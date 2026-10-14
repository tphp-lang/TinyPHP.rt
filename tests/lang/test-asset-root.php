<?php
// EXPECT: read=[]
// EXPECT: exists=false
// EXPECT: root=tphp://asset
// 未打包：ASSET_ROOT 读取返回 false（警告走 stderr，EXPECT 只比 stdout）
$v = file_get_contents(ASSET_ROOT . "/nope.txt");
echo "read=[", $v === false ? "" : $v, "]\n";
echo "exists=", file_exists(ASSET_ROOT . "/nope.txt") ? "true" : "false", "\n";
echo "root=", ASSET_ROOT, "\n";
