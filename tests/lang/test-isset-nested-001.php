<?php
// EXPECT: bool(false)
// EXPECT: bool(true)
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: bool(true)
// EXPECT: string(1) "d"
// EXPECT: isset-deep-ok

// ≥3 维 isset/empty/?? 链全程静默（PHP 语义：任意深度缺失键不 raise）。
// 修复点：compile.c compile_dim_read silent 递归（此前仅静默紧邻一层，
// 内层落非静默 FETCH_DIM_R → 中间键缺失 UndefinedError；未定义变量基
// 多层下标同理）。双端（php CLI vs tphp）对拍。
$x = [];
$a = ["k" => ["i" => ["n" => 1]]];
var_dump(isset($x["a"]["b"]["c"]));      // 中间键缺失（≥3 维核心案）
var_dump(isset($a["k"]["i"]["n"]));      // 全存在
var_dump(isset($a["k"]["i"]["z"]));      // 末层缺失
var_dump(isset($a["k"]["x"]["z"]));      // 中层缺失
var_dump(isset($a["m"]["i"]["n"]));      // 首层缺失
var_dump(isset($a["k"]["i"]["n"]["q"])); // 4 维：取到标量再下标
var_dump(isset($undef["a"]["b"]));       // 未定义变量基 + 多层
var_dump(empty($x["a"]["b"]));           // empty 深链
var_dump($x["a"]["b"] ?? "d");           // ?? 深链
echo "isset-deep-ok\n";
