<?php
// 刀15 take 瘦身（TPHP_TAKE_THIN，spec_c 路径 dimic_take 三免）边角对拍：
// - thin 主路径（全元素 INT 正常取值）与异常 deopt 共存——越界读 =
//   tphp UndefinedError throw（既有语义，PHP 为 Warning+NULL，见 PLAN/01
//   §10 差异清单；本测试按 tphp 三模对拍，不引 PHP 参考）；
// - guard-fail（str 元素）与 spec 路径交替轮转（preheader 消灭与 guard
//   失败 RETURN 线零改动）；
// - 重复调用 rc 稳定（preheader 恒发 gate 释放 TMP 遗留，不炸不漏）。
// 三模逐字一致：默认 / TPHP_TAKE_THIN=0 / TPHP_JIT=0。
// EXPECT: --- takethin ---
// EXPECT: b1=60 integer
// EXPECT: b2=caught
// EXPECT: b3=60 integer
// EXPECT: b4=15 integer
// EXPECT: b5=120000 integer
// EXPECT: END

echo "--- takethin ---\n";

$f = function ($a, $n) {
    $s = 0;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};

// 1) thin 主路径：全元素 INT 正常取值
$a = [10, 20, 30];
$v = $f($a, 3);
echo "b1=$v " . gettype($v) . "\n";

// 2) 越界 = UndefinedError throw——thin take 与异常 deopt 共存
$oob = "no-throw";
try {
    $oob = $f($a, 5);
} catch (Error $e) {
    $oob = "caught";
}
echo "b2=$oob\n";

// 3) 异常后同函数再跑正常形态（槽状态干净）
$v2 = $f($a, 3);
echo "b3=$v2 " . gettype($v2) . "\n";

// 4) guard-fail（str 元素）与 spec 路径轮转 10000 次——末次 = int 数组
$as = ["1", "2"];
$ai = [7, 8];
$r = 0;
for ($t = 0; $t < 10000; $t++) {
    $r = ($t % 2) ? $f($ai, 2) : $f($as, 2);
}
echo "b4=$r " . gettype($r) . "\n";

// 5) 前置字符串运算（TMP 槽可能遗留 STR）+ 重复调用——preheader 恒发
// gate 释放遗留，rc 稳定不炸不漏
$u = 0;
for ($t = 0; $t < 2000; $t++) {
    $pad = str_repeat("x", 8) . $t;
    $u += $f($a, 3) + strlen($pad) - strlen($pad);
}
echo "b5=$u " . gettype($u) . "\n";

echo "END\n";
