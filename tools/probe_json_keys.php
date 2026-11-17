<?php
// 复现：json_encode 嵌套数组时顶层键名被内层键覆盖
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
echo $s, "\n";
echo "has_scripts=", (str_contains($s, '"scripts":') ? "YES" : "NO"), "\n";
echo "has_init_at_top=", (str_contains($s, '"init":{') ? "YES" : "NO"), "\n";
