<?php
// EXPECT: sorted=1 3 7 19 42
// EXPECT: capture=ok
// EXPECT: ok
// FFI 回调 v1（PLAN/02 §4.5.1）：闭包 → 函数指针参数位置，libffi trampoline。
// Windows msvcrt qsort（Linux/macOS 走 libc，符号相同）。
if (PHP_OS_FAMILY === "Windows") {
    $libc = FFI::cdef("
        void qsort(void *base, size_t nmemb, size_t size,
                   int (*compar)(const void *, const void *));
    ", "msvcrt.dll");
} else {
    $libc = FFI::cdef("
        void qsort(void *base, size_t nmemb, size_t size,
                   int (*compar)(const void *, const void *));
    ", "libc.so.6");
}

$a = FFI::new("int[5]");
$a[0] = 42; $a[1] = 7; $a[2] = 19; $a[3] = 3; $a[4] = 1;

// 比较回调：收到两个 void*（CData 视图），cast 成 int* 后解引用（PHP 同款）
$libc->qsort($a, 5, FFI::sizeof("int"), function ($x, $y) {
    return $x->cast("int*")[0] <=> $y->cast("int*")[0];
});
echo "sorted=", $a[0], " ", $a[1], " ", $a[2], " ", $a[3], " ", $a[4], "\n";

// 捕获（tphp 为 by-value，无 &$ref）：捕获常量参与比较，验证闭包体
// 捕获槽与 trampoline 持有（同闭包两次传入均正常执行）
$base = 0;
$cmp = function ($x, $y) use ($base) {
    return ($x->cast("int*")[0] + $base) <=> ($y->cast("int*")[0] + $base);
};
$libc->qsort($a, 5, 4, $cmp);
$libc->qsort($a, 5, 4, $cmp);
echo "capture=ok\n";
echo "ok\n";
