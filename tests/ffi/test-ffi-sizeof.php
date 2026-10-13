<?php
// EXPECT: 4 4
// EXPECT: 8 8
// EXPECT: 1 1
// EXPECT: 2
// EXPECT: 8
// EXPECT: point_size=8
// EXPECT: off_x=0
// EXPECT: off_y=4
// EXPECT: ok

// sizeof / alignof 内建类型
echo FFI::sizeof("int"), " ", FFI::alignof("int"), "\n";
echo FFI::sizeof("double"), " ", FFI::alignof("double"), "\n";
echo FFI::sizeof("char"), " ", FFI::alignof("char"), "\n";
echo FFI::sizeof("short"), "\n";
echo FFI::sizeof("long long"), "\n";

// struct 布局
$ffi = FFI::cdef("
    struct Point { int x; int y; };
");
echo "point_size=", $ffi->sizeof("struct Point"), "\n";
echo "off_x=", $ffi->offsetof("struct Point", "x"), "\n";
echo "off_y=", $ffi->offsetof("struct Point", "y"), "\n";
echo "ok\n";
