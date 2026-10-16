<?php
// EXPECT: line=13
// EXPECT: file-ok=true
// EXPECT: dir-ok=true
// EXPECT: inc-line=4
// EXPECT: inc-file-ok=true
// EXPECT: inc-dir-ok=true

// __LINE__ / __FILE__ / __DIR__ folded at parse time (per-source-file).
// Packed or not they are the ORIGINAL source paths; io re-roots a disk-miss
// into the bundle. basename() is not implemented, so assert via substr.

echo "line=", __LINE__, "\n";
$f = __FILE__;
echo "file-ok=", (strpos($f, "test-magic-constants.php") !== false) ? "true" : "false", "\n";
$d = __DIR__;
echo "dir-ok=", (strpos($d, "lang") !== false) ? "true" : "false", "\n";

require "magic-inc.php";
