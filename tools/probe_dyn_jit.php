<?php
// 刀9 语义矩阵：动态调用兜底路径的 JIT 分发（call_callable 非.recv 路径）。
// 覆盖：零参/带参/捕获/arity 洞/多余实参/raise 上传/内部 try/deopt 干净回退/
//       嵌套动态调用/usort 比较器/字符串可调用(静态方法,分发跳过)/循环内首调。
// 每段输出唯一标记，四模（默认 / TPHP_DYN_JIT=0 / TPHP_NEST_CALL=1 / JIT=0）+ PHP 8.5 逐字对拍。

function raise_it($m) { throw new Exception($m); }

// 1. 零参闭包（bench 形态）：首调即分发，长循环体
$r1 = call_user_func(function () {
    $s = 0;
    for ($i = 0; $i < 100000; $i++) $s += $i;
    return $s;
});
echo "1 zero-arg sum=$r1\n";

// 2. 带参闭包：参数经队列拷贝进槽（poff 路径），返回值直传
$add = function ($a, $b) { return $a + $b; };
echo "2 args=", call_user_func($add, 20, 22), "\n";

// 3. 捕获 + 参数混合（poff = n_cap 布局）
$base = 100;
$capped = function ($x) use ($base) { return $base + $x; };
echo "3 cap=", call_user_func($capped, 23), "\n";

// 4. 实参洞（UNDEF）→ 解释器 RECV 默认值语义
$defed = function ($a, $b = 7) { return $a * 100 + $b; };
echo "4 hole=", call_user_func($defed, 3), "\n";

// 5. 多余实参 → 静默忽略（PHP 语义）
echo "5 extra=", call_user_func($add, 1, 2, 999), "\n";

// 6. raise 上传（JIT'd callee 内 raise → 分发异常支 LONGJMP → 外层 catch）
$boom = function () { raise_it("boom6"); };
try { call_user_func($boom); echo "6 MISS\n"; }
catch (Exception $e) { echo "6 caught=", $e->getMessage(), "\n"; }

// 7. JIT'd caller 动态调用首调 raise（bench 同构：$f() 在 JIT'd 函数内）
function dyn_raise_caller($f) { return $f(); }
function dyn_try() {
    try { dyn_raise_caller(function () { raise_it("boom7"); }); return "MISS"; }
    catch (Exception $e) { return "caught=" . $e->getMessage(); }
}
echo "7 ", dyn_try(), "\n";

// 8. callee 内部 try/catch（JIT'd 帧无 try → raise 必在解释器侧或上传；
//    本形态 callee 含 try → emit 拒编 → 纯解释器，验证不受分发影响）
$safe = function () { try { raise_it("inner8"); return "MISS"; } catch (Exception $e) { return "inner-caught"; } };
echo "8 ", call_user_func($safe), "\n";

// 9. 闭包循环内自捕获再调用（首次解释器编译 → 后续 trampoline 链）
function dyn_loop($f, $n) { $s = 0; for ($i = 0; $i < $n; $i++) $s += $f($i); return $s; }
echo "9 loop=", dyn_loop(function ($i) { return $i * 2; }, 1000), "\n";

// 10. usort 比较器（经典动态调用热路径）
$words = ["pear", "apple", "fig"];
usort($words, function ($a, $b) { return strlen($a) - strlen($b) ?: strcmp($a, $b); });
echo "10 usort=", implode(",", $words), "\n";

// 11. usort 比较器 raise → 上传
$bad = [3, 1, 2];
try { usort($bad, function ($a, $b) { if ($a === 1) raise_it("cmp11"); return $a - $b; }); echo "11 MISS\n"; }
catch (Exception $e) { echo "11 caught=", $e->getMessage(), "\n"; }

// 12. 嵌套动态调用（JIT'd callee 内再动态调 → 分发递归组合）
$inner = function ($x) { return $x + 1; };
$outer = function ($x) use ($inner) { return $inner($x) * 10; };
echo "12 nest=", call_user_func($outer, 4), "\n";

// 13. clean deopt：JIT'd 闭包内静态调 JIT'd 函数传非 INT（INT 契约 → 干净回退
//     → 解释器 RECV 重跑；验证槽副本 release + 队列引用 move 后值无损）
function deopt_target($n) { return $n . "!"; }
function deopt_caller($s) { return deopt_target($s); }
echo "13 deopt=", call_user_func(function ($s) { return deopt_caller($s); }, "xy"), "\n";

// 14. 字符串可调用（普通函数，非 method）→ 分发放行面
function k8_plain($x) { return $x * 3; }
echo "14 strfn=", call_user_func("k8_plain", 5), "\n";

// 15. 静态方法字符串可调用（is_method → 分发跳过，解释器语义不变）
class K8S { public static function sm($x) { return $x + 50; } }
echo "15 static=", call_user_func("K8S::sm", 8), "\n";

// 16. 长循环闭包内 raise（OSR 窗口中途异常 → 帧弹出无泄漏、上传正确）
function long_raise($n) {
    $acc = 0;
    for ($i = 0; $i < $n; $i++) {
        $acc += $i;
        if ($i === 50000) raise_it("mid16");
    }
    return $acc;
}
try { call_user_func(function () { long_raise(100000); }); echo "16 MISS\n"; }
catch (Exception $e) { echo "16 caught=", $e->getMessage(), "\n"; }

// 17. 引用计数压力：数组实参进出（队列引用消费/槽副本释放平衡）
$arr = [1, 2, 3];
$eat = function ($a) { $c = count($a); unset($a); return $c; };
echo "17 refc=", call_user_func($eat, $arr), " arr=", count($arr), "\n";

// 18. Closure::call 路径（同走 call_callable）
class K8C { public $v = 9; }
$c18 = function () { return $this->v * 2; };
echo "18 call=", $c18->call(new K8C()), "\n";

// 19. 生成器闭包不误分发（yield 形态 → 无 jit_entry → 解释器原语义）
$gen = function ($n) { for ($i = 0; $i < $n; $i++) yield $i; };
$s = 0; foreach ($gen(4) as $v) $s += $v;
echo "19 gen=$s\n";

echo "all-ok\n";
