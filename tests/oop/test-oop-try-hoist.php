<?php
// TRY-hoist: class/function inside try are unconditional (PHP semantics);
// conditional decls in if(false) are NOT defined (matches PHP 8).
// EXPECT: 1
// EXPECT: 42
// EXPECT: bool(false)
// EXPECT: ok

try {
    class InTry { public $a = 1; }
    function in_try_fn() { return 42; }
} catch (Throwable $e) {}

echo (new InTry())->a, "\n";
echo in_try_fn(), "\n";

if (false) {
    class InIf { public $b = 2; }
}
var_dump(class_exists("InIf"));
echo "ok\n";
