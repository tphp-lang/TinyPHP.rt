<?php
// EXPECT: enum_a=1
// EXPECT: enum_b=2
// EXPECT: enum_c=10
// EXPECT: ok

// FFI 对象上暴露 enum 常量（$ffi->RED）
$ffi = FFI::cdef("
    enum Color { RED = 1, GREEN = 2, BLUE = 10 };
");
echo "enum_a=", $ffi->RED, "\n";
echo "enum_b=", $ffi->GREEN, "\n";
echo "enum_c=", $ffi->BLUE, "\n";
echo "ok\n";
