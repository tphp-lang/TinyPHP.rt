<?php
// EXPECT: x=1
// EXPECT: y=2.5
// EXPECT: x2=10
// EXPECT: mixed=3.5
// EXPECT: ok

$ffi = FFI::cdef("
    struct Mixed { int a; double b; char c; };
");
$p = $ffi->new("struct Mixed");
$p->a = 1;
$p->b = 2.5;
$p->c = 65;
echo "x=", $p->a, "\n";
echo "y=", $p->b, "\n";
$p->a = 10;
echo "x2=", $p->a, "\n";
$p->b = 3.5;
echo "mixed=", $p->b, "\n";
echo "ok\n";
