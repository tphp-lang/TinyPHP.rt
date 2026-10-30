<?php
// 跨线程 FFI 回调（方案1 marshaling 固化）：外部 OS 线程触发 trampoline
// （无 VM）→ 投递归属线程事件循环执行 → 结果回传 C 调用方。
// 共享槽经「线程函数入参」解码写入（验 arg decode），返回值经各自的
// 取回通道读回（验 encode 回传：Windows=GetExitCodeThread 线程退出码，
// POSIX=pthread_join 的 void* 返回值再解引用）。
// EXPECT: on-main=ok
// EXPECT: ret=ok
if (PHP_OS_FAMILY === "Windows") {
    $ffi = FFI::cdef("
        typedef unsigned long (*thread_fn)(void *);
        void *CreateThread(void *sa, unsigned long stack, thread_fn start,
                           void *arg, unsigned long flags, unsigned long *id);
        unsigned long WaitForSingleObject(void *h, unsigned long ms);
        int GetExitCodeThread(void *h, unsigned long *code);
        int CloseHandle(void *h);
    ", "kernel32.dll");
    $slot = $ffi->new("unsigned long[1]");
    $fn = function ($arg) {
        $s = $arg->cast("unsigned long *");
        $s[0] = 42;
        echo "on-main=ok\n";
        return 42;
    };
    $h = $ffi->CreateThread(null, 0, $fn, $slot, 0, null);
    // 主线程挂起进事件循环：drain 在 usleep 的 uv 空隙里执行回调
    for ($i = 0; $i < 1000 && $slot[0] != 42; $i++) usleep(10000);
    $code = $ffi->new("unsigned long[1]");
    $ffi->WaitForSingleObject($h, 2000);
    $ffi->GetExitCodeThread($h, $code);
    echo "ret=", ($code[0] === 42 ? "ok" : "BAD($code[0])"), "\n";
    $ffi->CloseHandle($h);
} else {
    $lib = PHP_OS_FAMILY === "Darwin" ? "libSystem.B.dylib" : "libc.so.6";
    $ffi = FFI::cdef("
        typedef void *(*thread_fn)(void *);
        int pthread_create(unsigned long *t, void *attr,
                           thread_fn start, void *arg);
        int pthread_join(unsigned long t, void **ret);
    ", $lib);
    $slot = $ffi->new("unsigned long[1]");
    $fn = function ($arg) {
        $s = $arg->cast("unsigned long *");
        $s[0] = 42;
        echo "on-main=ok\n";
        return $arg;
    };
    $t = $ffi->new("unsigned long[1]");
    $r = $ffi->new("void *[1]");
    $ffi->pthread_create($t, null, $fn, $slot);
    for ($i = 0; $i < 1000 && $slot[0] != 42; $i++) usleep(10000);
    $ffi->pthread_join($t[0], $r);
    $v = $r[0];
    echo "ret=", ($v !== null && $v->cast("unsigned long *")[0] === 42
        ? "ok" : "BAD"), "\n";
}
