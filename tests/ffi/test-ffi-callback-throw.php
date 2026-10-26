<?php
// EXPECT: catch-ok
// EXPECT: 1 2 3
// EXPECT: uncaught-alive-ok
// EXPECT: nested-ok
// EXPECT: 1 5
// EXPECT: ok
// FFI 回调异常 v2-3（PLAN/02 §4.5.1）：回调内 try/catch 正常；
// 未捕获 = warning(stderr) + 零值返回，进程不死（对齐 PHP E_WARNING）。
// 警告走 stderr，run.ps1 不比对——EXPECT 只钉 stdout 存活标记。
if (PHP_OS_FAMILY === "Windows") {
    $libc = FFI::cdef("
        void qsort(void *base, size_t nmemb, size_t size,
                   int (*compar)(const void *, const void *));
    ", "msvcrt.dll");
} elseif (PHP_OS_FAMILY === "Darwin") {
    $libc = FFI::cdef("
        void qsort(void *base, size_t nmemb, size_t size,
                   int (*compar)(const void *, const void *));
    ", "libSystem.B.dylib");
} else {
    $libc = FFI::cdef("
        void qsort(void *base, size_t nmemb, size_t size,
                   int (*compar)(const void *, const void *));
    ", "libc.so.6");
}

// 1) 回调内 try/catch 完全正常：抛出被自己接住，排序结果正确
$a = FFI::new("int[3]");
$a[0] = 3; $a[1] = 1; $a[2] = 2;
$libc->qsort($a, 3, FFI::sizeof("int"), function ($x, $y) {
    try {
        throw new Exception("inner");
    } catch (Exception $e) {
        return $x->cast("int*")[0] <=> $y->cast("int*")[0];
    }
});
echo "catch-ok\n";
echo $a[0], " ", $a[1], " ", $a[2], "\n";

// 2) 未捕获：每次比较 warning + 返回零值（视作相等），qsort 跑完、进程活着
$b = FFI::new("int[3]");
$b[0] = 9; $b[1] = 4; $b[2] = 7;
$libc->qsort($b, 3, FFI::sizeof("int"), function ($x, $y) {
    throw new RuntimeException("escaped");
});
echo "uncaught-alive-ok\n";

// 3) 嵌套重入：比较器内再触发一次带 trampoline 的 qsort（第二层各自 local setjmp）
$c = FFI::new("int[2]");
$c[0] = 5; $c[1] = 1;
$inner = FFI::new("int[2]");
$inner[0] = 8; $inner[1] = 2;
$libc->qsort($c, 2, FFI::sizeof("int"), function ($x, $y) use ($libc, $inner) {
    // 内层 qsort：未捕获 throw 逃逸一层，被内层 trampoline 接住
    $libc->qsort($inner, 2, FFI::sizeof("int"), function ($p, $q) {
        throw new RuntimeException("inner-escaped");
    });
    try {
        throw new Exception("outer-caught");
    } catch (Exception $e) {
        return $x->cast("int*")[0] <=> $y->cast("int*")[0];
    }
});
echo "nested-ok\n";
echo $c[0], " ", $c[1], "\n";
echo "ok\n";
