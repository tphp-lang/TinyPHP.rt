<?php
// EXPECT: parse_ok=true
// EXPECT: node_size=16
// EXPECT: menu_size=16
// EXPECT: ok
// FFI 头子集：函数指针参数、无名参数、自引用 struct（PebView.h 触发的解析缺口）
$h = <<<C
typedef void *webview_t;
struct tray_menu { int id; void (*callback)(const void *ptr); };
typedef struct node { char *p; struct node *next; } node;
void dispatch(webview_t w, void (*fn)(webview_t, void *), void *arg);
int sum2(int, int);
C;
try {
    $ffi = FFI::cdef($h);
    echo "parse_ok=", $ffi !== null ? "true" : "false", "\n";
} catch (Throwable $e) {
    echo "parse_ok=false\n";
}
echo "node_size=", $ffi->sizeof("struct node"), "\n";
echo "menu_size=", $ffi->sizeof("struct tray_menu"), "\n";
echo "ok\n";
