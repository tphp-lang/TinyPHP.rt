<?php
// EXPECT: strlen=12
// EXPECT: ok

// char* 字段读写 + 指针
$ffi = FFI::cdef("
    struct Buf { char *s; int n; };
");
$b = $ffi->new("struct Buf");
$b->s = "hello world";  // TStr NUL 结尾，直接存指针
$b->n = 12;
echo "strlen=", $b->n, "\n";
echo "ok\n";
