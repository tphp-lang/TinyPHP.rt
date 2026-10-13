<?php
// EXPECT: union_size=8
// EXPECT: union_off=0
// EXPECT: a=1
// EXPECT: b_lessthan=ok
// EXPECT: nested_size=16
// EXPECT: ok

// union 布局：全部字段 offset 0，size = max
$ffi = FFI::cdef("
    union U { int a; double b; };
    struct S { union U u; int tail; };
");
echo "union_size=", $ffi->sizeof("union U"), "\n";
echo "union_off=", $ffi->offsetof("union U", "b"), "\n";

$u = $ffi->new("union U");
$u->a = 1;
echo "a=", $u->a, "\n";
// double 读回可能是位模式，只验证可访问不崩
$x = $u->b;
if (is_float($x) || is_int($x)) echo "b_lessthan=ok\n";
else echo "b_lessthan=fail\n";

echo "nested_size=", $ffi->sizeof("struct S"), "\n";
echo "ok\n";
