<?php
// 刀16 FETCH 值直达（TPHP_FETCH_FWD，MERGE phi 桥 + dst 槽免往返）边角对拍：
// - b1/b3 主路径：hit 线 phi 直达求和正确；
// - b2 越界异常：miss 线 deopt（helper raise）时 dst 槽恒 UNDEF，解释器
//   从 FETCH pc 重跑自写 dst——与 fwd 共存；
// - b4 溢出物化（函数级）：ADD 溢出 RETURN 前补写 tmp 槽，解释器从 ADD pc
//   重跑读到物化值 → double 结果与纯解释器逐位一致；
// - b5 溢出物化（OSR 热循环，>10000 backedge 后 chunk 内溢出）：验证
//   OSR resume at ADD pc 的物化路径；无论 OSR 是否介入，末值须一致；
// - b6 guard 轮转（str 元素 → fwd 不激活路径）；
// - b7 STR 遗留 + 重复调用 rc 稳定（dst 槽恒 UNDEF/标量，teardown no-op）。
// 三模逐字一致：默认 / TPHP_FETCH_FWD=0 / TPHP_JIT=0。
// EXPECT: --- fetchfwd ---
// EXPECT: b1=60 integer
// EXPECT: b2=caught
// EXPECT: b3=60 integer
// EXPECT: b4=double ok
// EXPECT: b5=double ok
// EXPECT: b6=15 integer
// EXPECT: b7=120000 integer
// EXPECT: END

echo "--- fetchfwd ---\n";

$f = function ($a, $n) {
    $s = 0;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};

// 1) 主路径：fwd phi 求和
$a = [10, 20, 30];
$v = $f($a, 3);
echo "b1=$v " . gettype($v) . "\n";

// 2) 越界 = UndefinedError throw——fwd 激活下 miss 线 deopt 共存
$oob = "no-throw";
try {
    $oob = $f($a, 5);
} catch (Error $e) {
    $oob = "caught";
}
echo "b2=$oob\n";

// 3) 异常后同函数再跑正常形态
$v2 = $f($a, 3);
echo "b3=$v2 " . gettype($v2) . "\n";

// 4) 溢出物化（函数级）：INT64_MAX + 1 → double，语义 = 解释器 int→float 相加
$ov = function ($n) {
    $s = 9223372036854775807;
    $i = 0;
    while ($i < $n) {
        $s += 1;
        $i++;
    }
    return $s;
};
$r4 = $ov(1);
echo "b4=" . (is_float($r4) && $r4 == 9223372036854775808.0 ? "double ok" : "bad:$r4") . "\n";

// 5) 溢出物化（OSR 热循环）：8e14/iter，第 ~11530 次迭代超 INT64_MAX 溢出
// （>OSR 阈值 10000，若 chunk 已编译则走物化 resume）；溢出后 double 连加
// 末值 9.6E+18——三模（默认/fwd=0/jit=0）逐位一致即证语义零漂移
$hot = function () {
    $s = 0;
    $i = 0;
    $v = 800000000000000;
    while ($i < 12000) {
        $s += $v;
        $i++;
    }
    return $s;
};
$r5 = $hot();
echo "b5=" . (is_float($r5) && $r5 == 9.6E+18 ? "double ok" : "bad:$r5") . "\n";

// 6) guard 轮转（str 元素 → spec/fwd 不激活）
$as = ["1", "2"];
$ai = [7, 8];
$r = 0;
for ($t = 0; $t < 10000; $t++) {
    $r = ($t % 2) ? $f($ai, 2) : $f($as, 2);
}
echo "b6=$r " . gettype($r) . "\n";

// 7) 前置字符串运算（TMP 槽可能遗留 STR）+ 重复调用——teardown rc 稳定
$u = 0;
for ($t = 0; $t < 2000; $t++) {
    $pad = str_repeat("x", 8) . $t;
    $u += $f($a, 3) + strlen($pad) - strlen($pad);
}
echo "b7=$u " . gettype($u) . "\n";

echo "END\n";
