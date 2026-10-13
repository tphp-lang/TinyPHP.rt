<?php
// lib/std.php - 基础工具（打进单 exe）
function str_title($s) {
    return ucwords(strtolower($s));
}

function arr_second($a) {
    $vals = array_values($a);
    return count($vals) >= 2 ? $vals[1] : null;
}

function clamp_num($x, $lo, $hi) {
    if ($x < $lo) return $lo;
    if ($x > $hi) return $hi;
    return $x;
}

const TPHP_LIB = "std";
