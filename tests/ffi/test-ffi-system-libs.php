<?php
// EXPECT: tick_ok
// EXPECT: kernel_ok
// EXPECT: ok

// user32 / kernel32 系统库零配置绑定
$u = FFI::cdef("
    unsigned int GetTickCount(void);
", "user32.dll");
$t = $u->GetTickCount();
if (is_int($t) && $t >= 0) echo "tick_ok\n";
else echo "tick_fail\n";

$k = FFI::cdef("
    void *GetCurrentProcess(void);
    unsigned long GetCurrentProcessId(void);
", "kernel32.dll");
$pid = $k->GetCurrentProcessId();
if (is_int($pid) && $pid > 0) echo "kernel_ok\n";
else echo "kernel_fail\n";
echo "ok\n";
