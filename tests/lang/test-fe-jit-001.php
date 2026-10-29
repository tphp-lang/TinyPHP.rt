<?php
// P0-14 批3（FE_RESET/FE_FETCH foreach 准入）：foreach 五场景对拍。
// FE 两 op 走 tphp_jit_opseq 单 op pad 回放（vm.c 派发表）；raise 经
// 有界 pad deopt 传播，catch 一律在 JIT 外（与 test-new-jit-001 同款）。
// EXPECT: 106
function fe_kv_sum() {
    $a = [10, 20, 30, 40];
    $s = 0;
    $ks = 0;
    foreach ($a as $k => $v) {
        $s += $v;
        $ks += $k;
    }
    return $s + $ks;    // 100 + (0+1+2+3) = 106
}
echo fe_kv_sum(), "\n";
// EXPECT: 7|ab
function fe_kv_str() {
    $a = ["a" => 3, "b" => 4];
    $s = 0;
    $ks = "";
    foreach ($a as $k => $v) {
        $s += $v;
        $ks .= $k;
    }
    return $s . "|" . $ks;
}
echo fe_kv_str(), "\n";
// EXPECT: 12
function fe_brcont() {
    $a = [1, 2, 3, 4, 5, 6, 7, 8];
    $s = 0;
    foreach ($a as $k => $v) {
        if ($k % 2 === 0) continue;
        if ($v >= 7) break;
        $s += $v;
    }
    return $s;          // 奇数位 2+4+6；v=8 时 break
}
echo fe_brcont(), "\n";
// EXPECT: 0
function fe_empty() {
    $a = [];
    $n = 0;
    foreach ($a as $v) {
        $n++;
    }
    return $n;
}
echo fe_empty(), "\n";
// EXPECT: TypeError:foreach iterates over value of type int
function fe_bad() {
    foreach (42 as $v) {
    }
}
try { fe_bad(); } catch (TypeError $e) { echo get_class($e), ":", $e->getMessage(), "\n"; }
// EXPECT: DivisionByZeroError
function fe_boom($x) { return 1 / $x; }
// 循环体内调用抛错：deopt 从 JIT 循环内传播（opseq/bcall 双门）
function fe_loop_raise() {
    $a = [1, 2, 0, 3];
    $s = 0;
    foreach ($a as $v) {
        $s += fe_boom($v);
    }
    return $s;
}
try { fe_loop_raise(); } catch (DivisionByZeroError $e) { echo get_class($e), "\n"; }
