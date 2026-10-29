<?php
// Regression: LOOP multi-concat with dst==src same-ref args (in-place .=
// after a concat chain) hit a deps/ir emit bug where MEM->REG loads were
// emitted after the REG->REG shuffle at CALLs (fixed in ir_emit_*).
// EXPECT: x60y90x60y90x60y90
function concat_loop_chain($i) {
    $o = "";
    for ($k = 0; $k < 3; $k++) {
        $m = 60;
        $o .= "x" . $m . "y" . $i;
    }
    return $o;
}
echo concat_loop_chain(90), "\n";
// EXPECT: abbb
function concat_loop_inplace($t) {
    for ($k = 0; $k < 3; $k++) {
        $t .= "b";
    }
    return $t;
}
echo concat_loop_inplace("a"), "\n";
// EXPECT: 6x=xxxxxx
function concat_loop_mixed($n) {
    $x = "";
    for ($k = 0; $k < $n; $k++) {
        $x = $x . "x";
    }
    return "6x=" . $x;
}
echo concat_loop_mixed(6), "\n";
