<?php
// type-spec 批4a（kv 形态解锁，PLAN/type-spec.md §8）边角对拍：
// - ht guard 主路径（hash 键基 + 全活跃桶值 INT，F1 packed 取键 → F2
//   hash 查表 → rmw ADD → INC 链）；
// - guard-fail（混合值 hash / packed 基 / 空数组）交解释器（批1 同策略）
//   ——结果语义 = 解释器 = PHP；
// - tombstone（unset 中间键，写路径清 all_int → guard 重扫）+ 循环内
//   absent raise+catch；
// - 数组重建（写 flag 失效）后再入 guard 重验；
// - 大表（64 键，非 interned 动态键 → IR probe 指针等失配恒落 C helper
//   内容比较兜底 = 正确性面）；
// - int64 溢出 spec 快线 RETURN → 解释器重跑落 float；
// - STR 键 rc 稳定（F1 take 每迭代 addref/release，1e5 次调用不炸不漏）；
// - 顶层 OSR 形态（guard 在 chunk entry / main 帧同语义）；
// - guard 成败轮转 5000 次（preheader 消灭与失败 RETURN 线交替）。
// 三模逐字一致：默认 / TPHP_TYPE_SPEC=0 / TPHP_JIT=0。
// EXPECT: --- typespec-kv ---
// EXPECT: k1=108 integer
// EXPECT: k2=7.5 double
// EXPECT: k3=caught
// EXPECT: k4=22 integer
// EXPECT: k5=6240 integer
// EXPECT: k6=double
// EXPECT: k7=450000 integer
// EXPECT: k8=36 integer
// EXPECT: k9=2500 integer
// EXPECT: k10=caught
// EXPECT: k11=caught
// EXPECT: END

echo "--- typespec-kv ---\n";

$mk8 = function () {
    return ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
            "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
};

// 1) ht guard 主路径：kv-full 形态（3 轮 × 8 键 = 3×36）
$f1 = function () {
    $m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
          "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
    $ks = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
    $s = 0;
    for ($r = 0; $r < 3; $r++) {
        for ($k = 0; $k < 8; $k++) $s += $m[$ks[$k]];
    }
    return $s;
};
$v = $f1();
echo "k1=$v " . gettype($v) . "\n";

// 2) 混合值 hash（含数字串/float）→ guard-fail 交解释器：0+1+"2"+3+1.5 = 7.5
$f2 = function () {
    $m = ["a" => 1, "b" => "2", "c" => 3, "d" => 1.5];
    $ks = ["a", "b", "c", "d"];
    $s = 0;
    for ($r = 0; $r < 1; $r++) {
        for ($k = 0; $k < 4; $k++) $s += $m[$ks[$k]];
    }
    return $s;
};
$v = $f2();
echo "k2=$v " . gettype($v) . "\n";

// 3) tombstone（unset 中间键）+ 循环内 absent raise → caught
//    （unset 清 all_int → guard 重扫（活跃 1,3 仍全 INT 通过）→ spec 下
//     "b" 缺失 → helper raise → deopt → 解释器同 raise → 上传 catch）
$f3 = function () {
    $m = ["a" => 1, "b" => 2, "c" => 3];
    unset($m["b"]);
    $ks = ["a", "b", "c"];
    $s = 0;
    for ($r = 0; $r < 2; $r++) {
        for ($k = 0; $k < 3; $k++) $s += $m[$ks[$k]];
    }
    return $s;
};
$r3 = "no-throw";
try {
    $f3();
} catch (Error $e) {
    $r3 = "caught";
}
echo "k3=$r3\n";

// 4) 数组重建（写路径清 flag → guard 重验通过）后再入：(5+6)×2 = 22
$f4 = function () {
    $m = ["a" => 1, "b" => 2];
    unset($m["a"]);
    unset($m["b"]);
    $m["c"] = 5;
    $m["d"] = 6;
    $ks = ["c", "d"];
    $s = 0;
    for ($r = 0; $r < 2; $r++) {
        for ($k = 0; $k < 2; $k++) $s += $m[$ks[$k]];
    }
    return $s;
};
$v = $f4();
echo "k4=$v " . gettype($v) . "\n";

// 5) 大表 64 键（动态非 interned 键 → IR 指针等失配恒落 helper 内容比较；
//    深碰撞 >4 探测同落 C 兜底）：sum(1..64)=2080 × 3 轮 = 6240
$f5 = function () {
    $m = [];
    $ks = [];
    for ($i = 1; $i <= 64; $i++) {
        $m["k$i"] = $i;
        $ks[] = "k$i";
    }
    $s = 0;
    for ($r = 0; $r < 3; $r++) {
        for ($k = 0; $k < 64; $k++) $s += $m[$ks[$k]];
    }
    return $s;
};
$v = $f5();
echo "k5=$v " . gettype($v) . "\n";

// 6) int64 溢出：spec 双 INT 快线溢出分支 RETURN → 解释器重跑 → double
//    （gettype 浮点 = tphp 既有 "double"，三模对拍不引 PHP 名）
$f6 = function () {
    $m = ["x" => 2];
    $ks = ["x"];
    $s = PHP_INT_MAX - 1;
    for ($r = 0; $r < 1; $r++) {
        for ($k = 0; $k < 1; $k++) $s += $m[$ks[$k]];
    }
    return $s;
};
$v = $f6();
echo "k6=" . gettype($v) . "\n";

// 7) STR 键 rc 稳定：kv-full 形态 1e5 次查表（F1 take 每迭代
//    addref/release），末值 12500×36 = 450000
$rc = 0;
$m7 = $mk8();
$ks7 = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
for ($r = 0; $r < 12500; $r++) {
    for ($k = 0; $k < 8; $k++) $rc += $m7[$ks7[$k]];
}
echo "k7=$rc " . gettype($rc) . "\n";

// 8) 顶层 OSR 形态（非闭包，guard 在 chunk entry / main 帧）
$m8 = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
       "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
$ks8 = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
$os = 0;
for ($r = 0; $r < 1; $r++) {
    for ($k = 0; $k < 8; $k++) $os += $m8[$ks8[$k]];
}
echo "k8=$os " . gettype($os) . "\n";

// 9) guard 成败轮转 5000 次（偶 f1=int map 过、奇 f2=mixed map 败），
//    计 f1 返回 108 的次数 = 2500（过/败线交替，preheader 消灭与
//    RETURN 线零交互）
$s9 = 0;
for ($t = 0; $t < 5000; $t++) {
    $ok = ($t % 2) ? $f2() : $f1();
    if ($ok === 108) $s9++;
}
echo "k9=$s9 " . gettype($s9) . "\n";

// 10) packed 基 2-fetch（guard_ht 拒 packed → 交解释器 → absent raise）
$f10 = function () {
    $p = [1, 2, 3];
    $ks = ["alpha"];
    $s = 0;
    for ($r = 0; $r < 1; $r++) {
        for ($k = 0; $k < 1; $k++) $s += $p[$ks[$k]];
    }
    return $s;
};
$r10 = "no-throw";
try {
    $f10();
} catch (Error $e) {
    $r10 = "caught";
}
echo "k10=$r10\n";

// 11) 空数组（guard 失败方向同批1）→ absent raise
$f11 = function () {
    $e = [];
    $ks = ["alpha"];
    $s = 0;
    for ($r = 0; $r < 1; $r++) {
        for ($k = 0; $k < 1; $k++) $s += $e[$ks[$k]];
    }
    return $s;
};
$r11 = "no-throw";
try {
    $f11();
} catch (Error $e) {
    $r11 = "caught";
}
echo "k11=$r11\n";

echo "END\n";
