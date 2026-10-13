<?php
// EXPECT: eol_ok=true
// EXPECT: int_max_ok=true
// EXPECT: int_min_ok=true
// EXPECT: fetch_in_const=true
// EXPECT: echo_line=你好, TinyPHP
// EXPECT: ok
$eol = PHP_EOL;
$eol_ok = ($eol === "\r\n" || $eol === "\n");
echo "eol_ok=", $eol_ok ? "true" : "false", "\n";
echo "int_max_ok=", PHP_INT_MAX === 9223372036854775807 ? "true" : "false", "\n";
$min = PHP_INT_MIN;
echo "int_min_ok=", (gettype($min) === "integer" && $min == -9223372036854775808) ? "true" : "false", "\n";
$fetched = PHP_EOL;
echo "fetch_in_const=", ($fetched === $eol) ? "true" : "false", "\n";
echo "echo_line=你好, TinyPHP", $eol;
echo "ok", $eol;
