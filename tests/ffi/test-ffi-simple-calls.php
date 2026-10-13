<?php
// EXPECT: abs=5
// EXPECT: labs=42
// EXPECT: atoi=1234
// EXPECT: strlen=5
// EXPECT: sizeof_int=4
// EXPECT: sizeof_ptr=8
// EXPECT: ok

$ffi = FFI::cdef("
    int abs(int j);
    long labs(long j);
    int atoi(const char *str);
    unsigned long strlen(const char *s);
", "msvcrt.dll");

echo "abs=" . $ffi->abs(-5) . "\n";
echo "labs=" . $ffi->labs(-42) . "\n";
echo "atoi=" . $ffi->atoi("1234") . "\n";
echo "strlen=" . $ffi->strlen("hello") . "\n";
echo "sizeof_int=" . FFI::sizeof("int") . "\n";
echo "sizeof_ptr=" . FFI::sizeof("void*") . "\n";
echo "ok\n";
