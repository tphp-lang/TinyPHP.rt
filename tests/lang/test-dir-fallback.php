<?php
// EXPECT: read=[]
// EXPECT: exists=false
// T14: __DIR__ 相对读是打包数据访问的主推写法。未打包时磁盘读，未命中返回
// false（警告走 stderr，EXPECT 只比 stdout）。打包后的磁盘→zip 回落由 CI 的
// Pack smoke 验证（本套件只跑未打包路径）。
$v = file_get_contents(__DIR__ . "/nope.txt");
echo "read=[", $v === false ? "" : $v, "]\n";
echo "exists=", file_exists(__DIR__ . "/nope.txt") ? "true" : "false", "\n";
