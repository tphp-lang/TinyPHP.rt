<?php
// 跨 Program thunk 回归（Bug B，2026-11-04）：动态 require 的类（独立
// Program）里 ctor 默认值 thunk 的 fn_idx 相对该 Program 的 funcs 表——
// 曾用 vm->prog（主表）查 → 错位到别的 fn → ArgumentCountError {closure}
// （teb autoload 链实锤）。同时覆盖 Bug A：enum case 半保留字名（Default）。
// EXPECT: default=0
// EXPECT: given=0
// EXPECT: ok

$f = __DIR__ . "/helpers/thunk_reqcls.php";
require $f;                                  // 非字面量 → 运行期独立 Program

$c = new Thrq\Web2();                        // ctor 默认值 thunk（跨 prog 查表）
echo "default=" . $c->v . "\n";
$c2 = new Thrq\Web2(Thrq\Feat::Default);     // 显式传参对照
echo "given=" . $c2->v . "\n";
echo "ok\n";
