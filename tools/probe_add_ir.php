<?php
// IR dump 走查：ADD 快线结构（TPHP_JIT_DEBUG=2 看 lower_binop 发了什么）
function f($a, $b) {
    $x = 0;
    for ($i = 0; $i < 5; $i++) {
        $x = $a + $b;
        $c = $a == $b;
    }
    return $x;
}
echo f(1, 2), "\n";
