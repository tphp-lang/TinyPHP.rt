<?php
// M5.5 FFI 验收（PLAN/02 §4.5 + PLAN/04 M5.5 行）+ 已知限制修复
// EXPECT: system_bind=ok
// EXPECT: scalar_call=ok
// EXPECT: cdata_rw=ok
// EXPECT: struct_layout=ok
// EXPECT: union_layout=ok
// EXPECT: wchar_ok=ok
// EXPECT: enum_const=ok
// EXPECT: owned_free=ok
// EXPECT: worker_block=ok
// EXPECT: preproc_rejected=ok
// EXPECT: bitfield_rejected=ok
// EXPECT: m5.5-acceptance-ok

use Tphp\Worker;

// 1) 系统库零配置绑定（user32/msvcrt）
$u = FFI::cdef("unsigned int GetTickCount(void);", "user32.dll");
if (is_int($u->GetTickCount())) echo "system_bind=ok\n";
else echo "system_bind=fail\n";

// 2) 标量调用 + 变参
$m = FFI::cdef("
    int abs(int j);
    int atoi(const char *str);
    int sprintf(char *str, const char *format, ...);
", "msvcrt.dll");
if ($m->abs(-7) === 7 && $m->atoi("42") === 42) echo "scalar_call=ok\n";
else echo "scalar_call=fail\n";

// 3) CData 数组读写
$p = FFI::new("int[3]");
$p[0] = 1; $p[1] = 2; $p[2] = 3;
if ($p[0] === 1 && $p[2] === 3 && FFI::sizeof($p) === 12) echo "cdata_rw=ok\n";
else echo "cdata_rw=fail\n";

// 4) struct 布局 + 字段访问
$ffi = FFI::cdef("struct Rect { int l; int t; int r; int b; };");
$r = $ffi->new("struct Rect");
$r->l = 0; $r->t = 1; $r->r = 2; $r->b = 3;
if ($ffi->sizeof("struct Rect") === 16 &&
    $ffi->offsetof("struct Rect", "r") === 8 &&
    $r->b === 3) echo "struct_layout=ok\n";
else echo "struct_layout=fail\n";

// 5) union 布局（全部 offset 0）
$uu = FFI::cdef("union U { int a; double b; };");
if ($uu->sizeof("union U") === 8) {
    $uv = $uu->new("union U");
    $uv->a = 42;
    if ($uv->a === 42) echo "union_layout=ok\n";
    else echo "union_layout=fail\n";
} else echo "union_layout=fail\n";

// 6) wchar_t（Windows UTF-16 = 2 字节）
if (FFI::sizeof("wchar_t") === 2) {
    $w = FFI::cdef("int lstrcmpW(const wchar_t *a, const wchar_t *b);", "kernel32.dll");
    if ($w->lstrcmpW("hi", "hi") === 0) echo "wchar_ok=ok\n";
    else echo "wchar_ok=fail\n";
} else echo "wchar_ok=fail\n";

// 7) FFI 对象上的 enum 常量
$e = FFI::cdef("enum { EA = 1, EB = 99 };");
if ($e->EA === 1 && $e->EB === 99) echo "enum_const=ok\n";
else echo "enum_const=fail\n";

// 8) owned 释放
$owned = FFI::new("char[32]");
FFI::free($owned);
echo "owned_free=ok\n";

// 9) 阻塞调用走 Worker（文档约定）
$w2 = new Worker(function () {
    $me = Worker::self();
    $ff = FFI::cdef("int abs(int j);", "msvcrt.dll");
    $me->post($ff->abs(-99));
});
if ($w2->recv() === 99) echo "worker_block=ok\n";
else echo "worker_block=fail\n";
$w2->stop();

// 10) 预处理拒绝
try {
    FFI::cdef("#define X 1\nint f(void);");
    echo "preproc_rejected=fail\n";
} catch (Throwable $ex) {
    echo "preproc_rejected=ok\n";
}

// 11) bitfield 拒绝
try {
    FFI::cdef("struct B { int a : 3; };");
    echo "bitfield_rejected=fail\n";
} catch (Throwable $ex) {
    echo "bitfield_rejected=ok\n";
}

echo "m5.5-acceptance-ok\n";
