<?php
// EXPECT: sprintf_n=13
// EXPECT: ok

// 变参：msvcrt sprintf
$ffi = FFI::cdef("
    int sprintf(char *str, const char *format, ...);
    unsigned long strlen(const char *s);
", "msvcrt.dll");

$buf = FFI::new("char[64]");
$n = $ffi->sprintf($buf, "printf test %d", 5);
echo "sprintf_n=", $n, "\n";
echo "ok\n";
