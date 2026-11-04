<?php
// bug1 回归（2026-11-04）：JIT'd caller 内动态调用 raise 闭包——异常穿 IR 帧。
// 修复前：tphp_jit_call_dyn 的解释器 fallback 裸调 tphp_vm_call_callable，
// callee raise 的 longjmp（RtlUnwind SEH 展开）穿过无 UNWIND_INFO 的 IR
// 机器码帧 → 0xC00000FF/AV（gdb 栈实锤）；JIT 激活时本形态必崩 = 强断言。
// 修复 = tphp_vm_call_guarded（vm.c）本地 pad + vm_unwind_core 以 floor
// 分发：callee 侧 try 命中 → resume 跑完返 1（bug2 用例 D）；逃逸 → 返 0
// → IR IF(ok==0) RETURN → op_do_fcall 异常 deopt 从 C 帧上传（IR 帧已
// 返回，零跨帧展开；bug1 用例 A-C）。
// EXPECT: caught-fallback
// EXPECT: caught-mixed
// EXPECT: caught-mixed2
// EXPECT: 41
// EXPECT: end
// EXPECT: fallback-catch:TypeError
// EXPECT: continued
function dyn_caller($fn) { return $fn(); }
// A: callee 含 try 被拒编 → 恒走解释器 fallback（修复路径本体）
$cb = function () { try { $x = [1]; return "$x|v"; } catch (TypeError $e) { throw $e; } };
try { dyn_caller($cb); echo "no-catch-a\n"; } catch (TypeError $e) { echo "caught-fallback\n"; }
// B: callee 可编译——覆盖 fallback 首调与（若触发编译）e() 直调分支
$cm = function () { $x = [2]; return "$x|v"; };
try { dyn_caller($cm); echo "no-catch-b1\n"; } catch (TypeError $e) { echo "caught-mixed\n"; }
try { dyn_caller($cm); echo "no-catch-b2\n"; } catch (TypeError $e) { echo "caught-mixed2\n"; }
// C: 正常动态调用对照（不 raise）
$ok = function () { return 41; };
echo dyn_caller($ok), "\n";
echo "end\n";
// D: bug2（11-04 下午）——callee 含 try（拒编）在自身帧接住 TypeError 后
// 正常返回、caller JIT 继续。修复前 tphp_vm_call_guarded 的裸 setjmp 版
// 缺分发：异常被外传给 caller deopt 链 → 主分发器 resume callee，但
// caller JIT 已退出（pc=0、args 被清 UNDEF）→ RECV 重跑报
// ArgumentCountError 误报、caller 后续代码全丢。
function dync2($l, $fn) { $r = $fn(); echo $l, ":", $r, "\n"; return "continued"; }
$ct = function () { try { $x = [9]; return "$x!"; } catch (TypeError $e) { return "TypeError"; } };
echo dync2("fallback-catch", $ct), "\n";
