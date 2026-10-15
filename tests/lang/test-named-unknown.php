<?php
// P2-A: unknown named parameter -> compile-time fatal (stderr; script never
// runs, stdout empty — same EXPECT protocol as test-deadlock.php)
function f($a = 1, $b = 2) { return [$a, $b]; }
var_dump(f(z: 9));

// EXPECT-EXIT: 255
