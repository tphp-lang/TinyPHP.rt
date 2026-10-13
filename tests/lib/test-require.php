<?php
// EXPECT: double=10
// EXPECT: greet=Hello, Tiny!
// EXPECT: ver=1.0
// EXPECT: ok

// M6: 字符串字面量 require 编译期内联；路径相对当前文件
require "helpers/util.php";

echo "double=", lib_double(5), "\n";
echo "greet=", lib_greet("Tiny"), "\n";
echo "ver=", LIB_VERSION, "\n";
echo "ok\n";
