<?php
// 刀5C 语义对拍矩阵：嵌套调用 emit 覆盖率。
// 四模逐字一致：默认（NEST_CALL 开）/ TPHP_NEST_CALL=0 / TPHP_JIT=0 / PHP 8.5。
// 重点风险：内层 builtin raise → 函数级 deopt 整函数重跑 → 参数表达式副作用双执行。

function n1($s) { return strlen(trim($s)); }
function n2($a, $b) { return max(min($a, $b), 0); }
function n3($s) { return str_repeat(strval($s), 2); }
function n4($s) { return sprintf("%s-%d", $s, 1); }
function n5($x) { return strlen(trim(strval($x))); }   // 三层嵌套：保守拒编
function brothers($a, $b) { return max(strlen($a), strlen($b)); }
function loopnest($n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) $s += strlen(trim("  x$i  "));
    return $s;
}
function sideeffect($s) { return strlen(trim($s . "!")); }
function incdec($i) { return strlen(strval($i++)) + $i; }

echo "n1=", n1("  ab  "), "\n";
echo "n2=", n2(3, 7), "\n";
echo "n3=", n3(12), "\n";
echo "n4=", n4("k"), "\n";
echo "n5=", n5(5), "\n";
echo "brothers=", brothers("abcd", "xy"), "\n";
echo "loopnest=", loopnest(2000), "\n";
echo "sideeffect=", sideeffect(" ab "), "\n";
echo "incdec=", incdec(7), "\n";

// 内层 builtin raise（数组传 strlen → TypeError），外层 try 接住；
// 参数表达式含副作用（$i++），验证 deopt 重跑不双执行。
function raisecase($i) {
    try {
        return strlen(array($i++));
    } catch (Throwable $e) {
        return "caught:" . $i;
    }
}
echo "raisecase=", raisecase(3), "\n";

// 多次调用（函数级 JIT 热路径反复进），结果稳定
echo "repeat=", n1(" q ") + n1("  w ") + n2(9, 4) + n3(2), "\n";

// 内层 builtin raise + 函数内可观察副作用（echo），try 在调用处让函数
// 保持可 JIT：验证 deopt 走异常上传而非整函数重跑（不双执行 = 只一个 E）。
function raiser($i) {
    echo "E";
    return strlen(trim(array($i++)));
}
try { raiser(3); } catch (Throwable $e) { echo "C"; }
echo "\n";
// EXPECT: n1=2
// EXPECT: n2=3
// EXPECT: n3=1212
// EXPECT: n4=k-1
// EXPECT: n5=1
// EXPECT: brothers=4
// EXPECT: loopnest=8890
// EXPECT: sideeffect=4
// EXPECT: incdec=9
// EXPECT: raisecase=caught:4
// EXPECT: repeat=28
// EXPECT: EC
