<?php
// EXPECT: arr0=10
// EXPECT: arr1=20
// EXPECT: arr2=30
// EXPECT: set_and_get=99
// EXPECT: size=12
// EXPECT: ok

$p = FFI::new("int[3]");
$p[0] = 10;
$p[1] = 20;
$p[2] = 30;
echo "arr0=", $p[0], "\n";
echo "arr1=", $p[1], "\n";
echo "arr2=", $p[2], "\n";
$p[0] = 99;
echo "set_and_get=", $p[0], "\n";
echo "size=", FFI::sizeof($p), "\n";
echo "ok\n";
