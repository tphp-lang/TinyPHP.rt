<?php
// EXPECT: wchar_size=2
// EXPECT: wstrcmp=ok
// EXPECT: ok

// wchar_t 在 Windows 为 2 字节（UTF-16）
echo "wchar_size=", FFI::sizeof("wchar_t"), "\n";

// 声明 wchar 参数函数（kernel32 lstrcmpW）并传 PHP 字符串
$ffi = FFI::cdef("
    int lstrcmpW(const wchar_t *a, const wchar_t *b);
", "kernel32.dll");
// 相同宽字符串比较应返回 0
$r = $ffi->lstrcmpW("abc", "abc");
if ($r === 0) echo "wstrcmp=ok\n";
else echo "wstrcmp=fail($r)\n";
echo "ok\n";
