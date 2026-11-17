<?php
// EXPECT: ok
// EXPECT: ok
// EXPECT: ok
// json_encode 嵌套数组键名隔离回归：
// 历史 bug —— arr_keys_map 共享单一键缓冲，递归编码子数组时覆写父层未遍历的键，
// 顶层 "scripts" 被内层最后键 "init" 顶替。修复：每层数组占独立段（单调栈）。

// 1. 原始复现：顶层最后键须保留，内层键不串到顶层
$a = [
    "name" => "tphp/tebview",
    "version" => "0.0.1",
    "type" => "ext",
    "ext" => [
        "sources" => ["src"],
        "include" => ["include"],
        "os" => [
            "windows" => [
                "cc" => "clang",
                "libs" => ["-ldwmapi", "-lgdi32"],
                "ldflags" => ["x.o"]
            ]
        ],
        "tphp_abi" => "0.0.2",
        "init" => "tphp_ext_tebview_init"
    ],
    "scripts" => [
        "postInstall" => "scripts/build.php",
        "postUpdate" => "scripts/build.php"
    ]
];
$s = json_encode($a);
echo (str_contains($s, '"scripts":{"postInstall"') && str_contains($s, '"init":"tphp_ext')) ? "ok\n" : "FAIL:$s\n";

// 2. realloc 扩容：5 层 × 4 键 = 25 段，触发 keys 缓冲 > 初始 16 后重分配，
//    父层段基址须在递归中重算（tm_realloc 可能移动缓冲）
$b = [
    "ka" => ["a1" => 1, "a2" => 2, "a3" => 3, "a4" => 4],
    "kb" => ["b1" => 1, "b2" => 2, "b3" => 3, "b4" => 4],
    "kc" => ["c1" => 1, "c2" => 2, "c3" => 3, "c4" => 4],
    "kd" => ["d1" => 1, "d2" => 2, "d3" => 3, "d4" => 4],
    "ke" => ["e1" => 1, "e2" => 2, "e3" => 3, "e4" => 4]
];
$s2 = json_encode($b);
echo (str_contains($s2, '"ke":{"e1":1') && str_contains($s2, '"ka":{"a1":1') && str_contains($s2, '"e4":4')) ? "ok\n" : "FAIL:$s2\n";

// 3. 深层嵌套：父 / 子 / 孙三层关联键各自独立
$c = [
    "top" => [
        "mid" => [
            "leaf1" => 1,
            "leaf2" => 2
        ],
        "mid2" => 3
    ],
    "top2" => 4
];
$s3 = json_encode($c);
echo (str_contains($s3, '"top":{"mid":{"leaf1":1') && str_contains($s3, '"top2":4')) ? "ok\n" : "FAIL:$s3\n";
