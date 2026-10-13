<?php
// M6 验收（PLAN/02 §4 + PLAN/04 M6 行）
// EXPECT: json_ok=ok
// EXPECT: date_ok=ok
// EXPECT: sprintf_ok=ok
// EXPECT: array_cb_ok=ok
// EXPECT: require_file=ok
// EXPECT: bundle_ok=ok
// EXPECT: m6-acceptance-ok

// 1) JSON
$j = json_encode(["a" => 1, "b" => [true, null]]);
$b = json_decode($j);
if ($b["a"] === 1 && $b["b"][0] === true && $b["b"][1] === null)
    echo "json_ok=ok\n";
else
    echo "json_ok=fail\n";

// 2) date
if (date("Y-m-d", 0) === "1970-01-01" && mktime(0, 0, 0, 1, 1, 1970) === 0)
    echo "date_ok=ok\n";
else
    echo "date_ok=fail\n";

// 3) sprintf
$s = sprintf("%s=%d (%.1f)", "n", 42, 3.14);
if ($s === "n=42 (3.1)") echo "sprintf_ok=ok\n";
else echo "sprintf_ok=fail($s)\n";

// 4) array callbacks
$m = array_map(fn($x) => $x + 1, [1, 2, 3]);
$f = array_filter([1, 2, 3, 4], fn($x) => $x > 2);
if ($m[2] === 4 && count($f) === 2) echo "array_cb_ok=ok\n";
else echo "array_cb_ok=fail\n";

// 5) require 文件系统（helpers）
require "helpers/util.php";
if (lib_double(21) === 42) echo "require_file=ok\n";
else echo "require_file=fail\n";

// 6) bundle 内嵌模块（单 exe）
require "std";
if (str_title("HELLO") === "Hello" && clamp_num(1, 5, 10) === 5)
    echo "bundle_ok=ok\n";
else
    echo "bundle_ok=fail\n";

echo "m6-acceptance-ok\n";
