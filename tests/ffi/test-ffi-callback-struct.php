<?php
// EXPECT: field-write-ok
// EXPECT: field-read-ok
// EXPECT: dedup-same-addr
// EXPECT: static-new-throws
// EXPECT: reuse-qsort-ok
// EXPECT: repeat-stable
// FFI 回调 v2（PLAN/02 §4.5.1）：struct 字段闭包赋值 + trampoline 去重。
// 内存安全：trampoline+闭包 TLS 表脚本级保活（非 owned 视图读回）；
// closure_create 失败路径 raise 前释放；同闭包同签名复用同一 trampoline。
$lib = PHP_OS_FAMILY === "Windows" ? "msvcrt.dll"
     : (PHP_OS_FAMILY === "Darwin" ? "libSystem.B.dylib" : "libc.so.6");
$ffi = FFI::cdef("
    struct S { int n; int (*cb)(const void *, const void *); };
    void qsort(void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));
", $lib);

$s = $ffi->new("struct S");
$s->n = 7;
echo $s->n === 7 ? "field-write-ok\n" : "field-WRITE-FAIL\n";

// 字段写闭包 → 读回非 owned 函数指针视图 → 地址非零
$cmp = function ($x, $y) {
    return $x->cast("int*")[0] <=> $y->cast("int*")[0];
};
$s->cb = $cmp;
$cb = $s->cb;
$addr = $cb->getAddress();
echo ($addr !== 0 && gettype($cb) !== "NULL")
    ? "field-read-ok\n" : "field-READ-FAIL\n";

// 去重：同闭包 + 同签名 → 同一 trampoline 地址（v2②）
$s2 = $ffi->new("struct S");
$s2->cb = $cmp;
$addr2 = $s2->cb->getAddress();
echo $addr2 === $addr ? "dedup-same-addr\n" : "dedup-FAIL\n";

// 静态 FFI::new("struct S")：明确 ValueError 而非崩溃（sc=NULL 防护）
$threw = false;
try { FFI::new("struct S"); }
catch (ValueError $e) { $threw = true; }
echo $threw ? "static-new-throws\n" : "static-new-FAIL\n";

// 字段里的 trampoline 仍是可调用的 C 函数指针：读回直接传 qsort
$a = FFI::new("int[3]");
$a[0] = 3; $a[1] = 1; $a[2] = 2;
$ffi->qsort($a, 3, 4, $cb);
echo ($a[0] === 1 && $a[1] === 2 && $a[2] === 3)
    ? "reuse-qsort-ok\n" : "reuse-qsort-FAIL\n";

// 循环写字段 200 次：去重后内存平稳（无崩溃即基线；TLS 表不线性膨胀）
for ($i = 0; $i < 200; $i++) $s->cb = $cmp;
echo ($s->cb->getAddress() === $addr) ? "repeat-stable\n" : "repeat-FAIL\n";
