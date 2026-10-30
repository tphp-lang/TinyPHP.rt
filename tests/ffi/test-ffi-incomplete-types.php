<?php
// FFI 不完整结构体指针 + 函数指针 typedef（bug-ffi-incomplete-struct-fnptr 修复固化）
// 8 形态实证表对拍真 PHP + 按值不完整拦截 + CData 字符串保活（非 interned rvalue）
// EXPECT: 1: OK
// EXPECT: 2: OK
// EXPECT: 3: OK
// EXPECT: 4: OK
// EXPECT: 5: OK
// EXPECT: 6: OK
// EXPECT: 7: OK
// EXPECT: 8: OK
// EXPECT: 9: OK
// EXPECT: 10: OK
// EXPECT: v1: ValueError
// EXPECT: v2: ValueError
// EXPECT: v3: ValueError
// EXPECT: keep: ok
function t($label, $code) {
    try { FFI::cdef($code); return "OK"; }
    catch (Throwable $e) { return get_class($e); }
}
echo "1: ", t("x", "void *make(void);"), "\n";
echo "2: ", t("x", "struct foo{int x;}; struct foo *make(void);"), "\n";
echo "3: ", t("x", "struct foo;"), "\n";
echo "4: ", t("x", "struct foo; struct foo *make(void);"), "\n";
echo "5: ", t("x", "typedef struct foo *foo_p; foo_p make(void);"), "\n";
echo "6: ", t("x", "struct foo { int x; struct foo *next; };"), "\n";
echo "7: ", t("x", "typedef int (*cb)(void*,void*); void set(cb f);"), "\n";
echo "8: ", t("x", "long long add(long long,long long);"), "\n";
echo "9: ", t("x", "struct fw; void f(const struct fw *p); struct fw *g(void);"), "\n";
echo "10: ", t("x", "typedef void (*cb2)(int); struct cbh { cb2 on_event; };"), "\n";
// 按值不完整 = 拦截（C 同非法）
echo "v1: ", t("x", "struct inc1; void f(struct inc1 v);"), "\n";
echo "v2: ", t("x", "struct inc2; struct inc2 make(void);"), "\n";
echo "v3: ", t("x", "struct inc3; struct h3 { struct inc3 s; };"), "\n";
// 字符串保活：rvalue 拼接串写入 char* 元素 → 强制 GC 后再让 C 侧 strcpy
// 读该指针（无保活 = 悬垂读，返回串内容错乱/崩；有保活 = 原值）
$ffi = FFI::cdef("char *strcpy(char *dest, const char *src);");
$dest = $ffi->new("char[64]");
$arr = $ffi->new("char *[2]");
$arr[0] = str_repeat("listening_ports", 1) . "";   // 非字面量新串（rc=1 rvalue）
$arr[1] = null;
gc_collect_cycles();
$got = $ffi->strcpy($dest, $arr[0]);
echo "keep: ", ($got === "listening_ports" ? "ok" : "BAD($got)"), "\n";
