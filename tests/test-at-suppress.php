<?php
// EXPECT: v1=T
// EXPECT: e1=caught
// EXPECT: v2=T
// EXPECT: n=7 loop=ok p=T
// EXPECT: END
// @ 抑制运算符（11-10 P1）：stdout 语义三端对拍（默认/JIT=0/PHP）。
// warning 的出现/消失走 stderr——runner 不比 stderr，抑制本身于
// debug 验证：裸调用 warning 两行出现、@ 包裹的 500 次循环零 warning
// （人工核；机制 = tphp_warn 统一出口查 vm.x.silence）。
// 1) 裸调用：warning 应在（收口未误杀——值 false）
$a = file_get_contents("no_such_aaa.txt");
echo "v1=", ($a === false ? "T" : "F"), "\n";
// 2) @ 不吞 Error：DivisionByZero 照抛被 catch
$e = "no";
try { @(1 % 0); } catch (\DivisionByZeroError $x) { $e = "caught"; }
echo "e1=$e\n";
// 3) 同帧 catch 后 silence 已归 0（unwind 兜底）：后续裸调用 warning 复现
$b = file_get_contents("no_such_bbb.txt");
echo "v2=", ($b === false ? "T" : "F"), "\n";
// 4) 嵌套 @（深度计数）+ 循环内 @（JIT SUP op 直发）+ @ 作实参位
$n = @(1 + @(2 * 3));
$ok = "ok";
for ($i = 0; $i < 500; $i++) { $x = @fopen("bad://$i", "r"); if ($x !== false) $ok = "bad"; }
$p = @file_get_contents("nope_$n.txt");
echo "n=$n loop=$ok p=", ($p === false ? "T" : "F"), "\n";
echo "END\n";
