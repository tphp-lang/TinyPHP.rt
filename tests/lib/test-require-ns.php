<?php
// EXPECT: fq=3
// EXPECT: usehit=3
// EXPECT: cls=App
// require 内联的 namespace 不得泄漏到后续语句（PHP 按文件划分命名空间）
require "ns-lib.php";
echo "fq=", NsLib\C::VAL, "\n";
use NsLib\C;
echo "usehit=", C::VAL, "\n";
class App {}
echo "cls=", get_class(new App()), "\n";
