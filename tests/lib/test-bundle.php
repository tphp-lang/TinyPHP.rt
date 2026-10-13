<?php
// EXPECT: title=Hello World
// EXPECT: second=2
// EXPECT: clamp=5
// EXPECT: clamp2=10
// EXPECT: lib=std
// EXPECT: ok

// require bundle 模块名（无路径/扩展名）
require "std";

echo "title=", str_title("HELLO world"), "\n";
echo "second=", arr_second([1, 2, 3]), "\n";
echo "clamp=", clamp_num(3, 5, 10), "\n";
echo "clamp2=", clamp_num(15, 5, 10), "\n";
echo "lib=", TPHP_LIB, "\n";
echo "ok\n";
