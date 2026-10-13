<?php
// EXPECT: roundtrip_ok
// EXPECT: nested_ok
// EXPECT: escape_ok
// EXPECT: ok

// roundtrip
$src = ["name" => "tphp", "ver" => 1, "ok" => true, "pi" => 3.25, "tags" => ["a", "b"]];
$s = json_encode($src);
$back = json_decode($s);
if ($back["name"] === "tphp" && $back["ver"] === 1 && $back["ok"] === true &&
    $back["pi"] === 3.25 && $back["tags"][1] === "b")
    echo "roundtrip_ok\n";
else
    echo "roundtrip_fail: $s\n";

// nested
$n = json_decode('{"a":{"b":[1,{"c":2}]}}');
if ($n["a"]["b"][1]["c"] === 2) echo "nested_ok\n";
else echo "nested_fail\n";

// escapes
$e = json_decode('"line1\nline2\ttab\\"q"');
if ($e === "line1\nline2\ttab\"q") echo "escape_ok\n";
else echo "escape_fail\n";

echo "ok\n";
