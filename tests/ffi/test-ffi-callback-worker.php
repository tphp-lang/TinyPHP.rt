<?php
// EXPECT: worker-qsort=1 3 7 19 42
// EXPECT: main-qsort=2 4 8
// EXPECT: ok
// FFI 回调 × Worker（PLAN/02 §4.5.1 线程约定）：
//   同线程同步调用；C 跨线程调回调 = 未定义（文档禁止）；Worker 隔离
//   是官方答案——每线程独立 TLS 回调表，Worker 内自建 cdef+trampoline。
use tphp\Worker;

$lib = PHP_OS_FAMILY === "Windows" ? "msvcrt.dll"
     : (PHP_OS_FAMILY === "Darwin" ? "libSystem.B.dylib" : "libc.so.6");
$qsort_decl = "
    void qsort(void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));
";

// 1) Worker 线程内完整回调链：cdef + 闭包 trampoline + qsort
$w = new Worker(function () use ($lib, $qsort_decl) {
    $me = Worker::self();
    $ffi = FFI::cdef($qsort_decl, $lib);
    $a = FFI::new("int[5]");
    $a[0] = 42; $a[1] = 7; $a[2] = 19; $a[3] = 3; $a[4] = 1;
    $ffi->qsort($a, 5, 4, function ($x, $y) {
        return $x->cast("int*")[0] <=> $y->cast("int*")[0];
    });
    $me->post("worker-qsort={$a[0]} {$a[1]} {$a[2]} {$a[3]} {$a[4]}");
});
echo $w->recv(), "\n";
$w->stop();

// 2) 主线程回调不受 Worker 影响（TLS 表每线程独立）
$ffi = FFI::cdef($qsort_decl, $lib);
$b = FFI::new("int[3]");
$b[0] = 8; $b[1] = 2; $b[2] = 4;
$ffi->qsort($b, 3, 4, function ($x, $y) {
    return $x->cast("int*")[0] <=> $y->cast("int*")[0];
});
echo "main-qsort={$b[0]} {$b[1]} {$b[2]}\n";
echo "ok\n";
