<?php
// 刀6（dimic probe0 sequential probe 回退）语义对拍矩阵。
// 三模必须逐字一致：默认（IC_PROBE 开）/ TPHP_IC_PROBE=0 / TPHP_JIT=0；
// 并与 PHP 8.5 原生对照。覆盖开放寻址探测链的全部边界：
//  - 8 键轮转（CI kv-read 形态，满表哈希冲突 → probe1..3）
//  - unset tombstone 后继续探测（val_off==UINT32_MAX 跳过）
//  - 数组 grow/rehash 后 IC 快照失效重建（bpending/grow）
//  - INT 键（非 STR 门，链零改动路径）/ 数字串键（norm 成 int）
//  - 超探测步数（>4）落 case 链 → helper → C 侧 arr_find 兜底
//  - refcounted 值（字符串）走 dimic_take 的 addref/release 门

function kvsum($m, $keys, $n) {
    $s = 0;
    $nk = count($keys);
    for ($r = 0; $r < $n; $r++) {
        for ($k = 0; $k < $nk; $k++) $s += $m[$keys[$k]];
    }
    return $s;
}
function kvstr($m, $keys, $n) {
    $s = 0;
    $nk = count($keys);
    for ($r = 0; $r < $n; $r++) {
        for ($k = 0; $k < $nk; $k++) $s += strlen($m[$keys[$k]]);
    }
    return $s;
}

$m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
      "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
$keys = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
echo "full=", kvsum($m, $keys, 20000), "\n";

// unset tombstone 后再读剩余键（探测链必须跳过墓碑继续）
$m2 = $m; unset($m2["gamma"]); unset($m2["epsilon"]);
$keys2 = ["alpha", "beta", "delta", "zeta", "eta", "theta"];
echo "tomb=", kvsum($m2, $keys2, 20000), "\n";

// 重新插入 → grow/rehash → IC 快照失效重建
$m3 = [];
for ($i = 0; $i < 64; $i++) $m3["k$i"] = $i;
$gk = ["k0", "k7", "k15", "k31", "k63"];
echo "grow=", kvsum($m3, $gk, 20000), "\n";

// INT 键（非 STR 门）
$mi = [0 => 11, 1 => 22, 2 => 33, 3 => 44];
echo "int=", kvsum($mi, [0, 1, 2, 3], 20000), "\n";

// 数字串键（PHP norm： "8" → int 8；"08" 保留字符串）
$mn = ["8" => 10, "08" => 20, "9" => 30];
echo "numstr=", kvsum($mn, ["8", "08", "9"], 20000), "\n";

// refcounted 值：dimic_take addref/release 门 + 探测命中
$ms = ["alpha" => "aa", "beta" => "bbb", "gamma" => "c", "delta" => "dddd",
       "epsilon" => "e", "zeta" => "ff", "eta" => "ggg", "theta" => "h"];
echo "strval=", kvstr($ms, $keys, 20000), "\n";

// 多次独立调用：IC 快照跨调用轮转（同一个 site 多态）
echo "repeat=", kvsum($m, $keys, 5000) + kvsum($m2, $keys2, 5000), "\n";
// EXPECT: full=720000
// EXPECT: tomb=560000
// EXPECT: grow=2320000
// EXPECT: int=2200000
// EXPECT: numstr=1200000
// EXPECT: strval=340000
// EXPECT: repeat=320000
