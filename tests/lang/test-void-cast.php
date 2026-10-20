<?php
// EXPECT: NULL
// EXPECT: ok

// PHP 8.5 (void) cast：求值后丢弃，表达式值为 null
function f() { return 42; }
$r = (void)f();
var_dump($r);
echo "ok\n";
