<?php
// P0-14 批4（ISSET_DIM 准入）：isset($a[$k]) 单 op pad 回放（vm.c
// tphp_jit_opseq 派发表）。真 handler 用 rd raw read——UNDEF 安全，
// isset 语义本就静默，全程零 raise（无 deopt 门需求）。
// EXPECT: 1
function idim_hit() {
    $a = ["x" => 5];
    return isset($a["x"]) ? 1 : 0;
}
echo idim_hit(), "\n";
// EXPECT: 0
function idim_miss() {
    $a = ["x" => 5];
    return isset($a["y"]) ? 1 : 0;
}
echo idim_miss(), "\n";
// EXPECT: 0
function idim_undef() {
    // base 为 UNDEF CV：raw read 不抛，直接 false（isset 语义）
    return isset($nope[0]) ? 1 : 0;
}
echo idim_undef(), "\n";
// EXPECT: 1|0
function idim_str() {
    $s = "hello";
    // 字符串偏移分支：i∈[0,len) true，越界 false
    return (isset($s[1]) ? 1 : 0) . "|" . (isset($s[9]) ? 1 : 0);
}
echo idim_str(), "\n";
// EXPECT: 3
function idim_loop($n) {
    $a = [3, 7, 11];
    $c = 0;
    for ($i = 0; $i < $n; $i++) {
        // 循环体内 isset（无 continue 形态）：回放在 loopcond 结构内
        if (isset($a[$i])) {
            $c++;
        }
    }
    return $c;   // i=0..4 中 0,1,2 命中
}
echo idim_loop(5), "\n";
// EXPECT: yes|no
function idim_int_key() {
    $a = [10 => 1];
    $k = 10;
    $x = isset($a[$k]) ? "yes" : "no";
    $k = 11;
    $y = isset($a[$k]) ? "yes" : "no";
    return $x . "|" . $y;
}
echo idim_int_key(), "\n";
