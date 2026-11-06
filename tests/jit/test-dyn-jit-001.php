<?php
// 刀9 语义矩阵：动态调用兜底路径的 JIT 分发（call_callable 非 recv 路径）
// + call_user_func 族守卫 pad（callee 内部 catch 不再挂死）。
// 五模逐字一致：默认 / TPHP_DYN_JIT=0 / TPHP_NEST_CALL=1 / TPHP_JIT=0 / PHP 8.5
// （唯一例外 case 11：usort 比较器异常 PHP 8.5 吞掉、tphp 传播——既有差异
//   见 backlog P2；本测试 EXPECT 按 tphp 语义锁定，防的就是回归成挂死/崩溃）。

function raise_it($m) { throw new Exception($m); }

// 1. 零参闭包（bench 形态）：首调即分发，长循环体
$r1 = call_user_func(function () {
    $s = 0;
    for ($i = 0; $i < 100000; $i++) $s += $i;
    return $s;
});
echo "1 zero-arg sum=$r1\n";
// EXPECT: 1 zero-arg sum=4999950000

$add = function ($a, $b) { return $a + $b; };
echo "2 args=", call_user_func($add, 20, 22), "\n";
// EXPECT: 2 args=42

$base = 100;
$capped = function ($x) use ($base) { return $base + $x; };
echo "3 cap=", call_user_func($capped, 23), "\n";
// EXPECT: 3 cap=123

$defed = function ($a, $b = 7) { return $a * 100 + $b; };
echo "4 hole=", call_user_func($defed, 3), "\n";
// EXPECT: 4 hole=307

echo "5 extra=", call_user_func($add, 1, 2, 999), "\n";
// EXPECT: 5 extra=3

$boom = function () { raise_it("boom6"); };
try { call_user_func($boom); echo "6 MISS\n"; }
catch (Exception $e) { echo "6 caught=", $e->getMessage(), "\n"; }
// EXPECT: 6 caught=boom6

function dyn_raise_caller($f) { return $f(); }
function dyn_try() {
    try { dyn_raise_caller(function () { raise_it("boom7"); }); return "MISS"; }
    catch (Exception $e) { return "caught=" . $e->getMessage(); }
}
echo "7 ", dyn_try(), "\n";
// EXPECT: 7 caught=boom7

// 8. callee 内部 try/catch（含 try → emit 拒编 → 纯解释器 + 守卫 pad）；
//    修复前：call_user_func 无 pad → 主分发器内部分恢复后挂死（JIT=0 同挂）
$safe = function () { try { raise_it("inner8"); return "MISS"; } catch (Exception $e) { return "inner-caught"; } };
echo "8 ", call_user_func($safe), "\n";
// EXPECT: 8 inner-caught

function dyn_loop($f, $n) { $s = 0; for ($i = 0; $i < $n; $i++) $s += $f($i); return $s; }
echo "9 loop=", dyn_loop(function ($i) { return $i * 2; }, 1000), "\n";
// EXPECT: 9 loop=999000

$words = ["pear", "apple", "fig"];
usort($words, function ($a, $b) { return strlen($a) - strlen($b) ?: strcmp($a, $b); });
echo "10 usort=", implode(",", $words), "\n";
// EXPECT: 10 usort=fig,pear,apple

$bad = [3, 1, 2];
try { usort($bad, function ($a, $b) { if ($a === 1) raise_it("cmp11"); return $a - $b; }); echo "11 MISS\n"; }
catch (Exception $e) { echo "11 caught=", $e->getMessage(), "\n"; }
// EXPECT: 11 caught=cmp11

$inner = function ($x) { return $x + 1; };
$outer = function ($x) use ($inner) { return $inner($x) * 10; };
echo "12 nest=", call_user_func($outer, 4), "\n";
// EXPECT: 12 nest=50

// 13. clean deopt：JIT'd 闭包内静态调 JIT'd 函数传非 INT（INT 契约 → 干净
//     回退 → 解释器 RECV release 槽副本 + move 队列引用，值无损）
function deopt_target($n) { return $n . "!"; }
function deopt_caller($s) { return deopt_target($s); }
echo "13 deopt=", call_user_func(function ($s) { return deopt_caller($s); }, "xy"), "\n";
// EXPECT: 13 deopt=xy!

function k8_plain($x) { return $x * 3; }
echo "14 strfn=", call_user_func("k8_plain", 5), "\n";
// EXPECT: 14 strfn=15

class K8S { public static function sm($x) { return $x + 50; } }
echo "15 static=", call_user_func("K8S::sm", 8), "\n";
// EXPECT: 15 static=58

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
// EXPECT: 16 caught=mid16

$arr = [1, 2, 3];
$eat = function ($a) { $c = count($a); unset($a); return $c; };
echo "17 refc=", call_user_func($eat, $arr), " arr=", count($arr), "\n";
// EXPECT: 17 refc=3 arr=3

class K8C { public $v = 9; }
$c18 = function () { return $this->v * 2; };
echo "18 call=", $c18->call(new K8C()), "\n";
// EXPECT: 18 call=18

$gen = function ($n) { for ($i = 0; $i < $n; $i++) yield $i; };
$s = 0; foreach ($gen(4) as $v) $s += $v;
echo "19 gen=$s\n";
// EXPECT: 19 gen=6

// 20. call_user_func_array（守卫同族）：正常 + variadic 展开 + callee 内部 catch + 逃逸
function k8_vari(...$a) { return count($a); }
echo "20 cufa=", call_user_func_array($add, [5, 6]), "|", call_user_func_array("k8_vari", [1, 2, 3]), "\n";
// EXPECT: 20 cufa=11|3
$cufa_safe = function () { try { raise_it("in20"); return "MISS"; } catch (Exception $e) { return "cufa-caught"; } };
echo "21 cufa-catch=", call_user_func_array($cufa_safe, []), "\n";
// EXPECT: 21 cufa-catch=cufa-caught
try { call_user_func_array(function () { raise_it("out20"); }, []); echo "22 MISS\n"; }
catch (Exception $e) { echo "22 escaped=", $e->getMessage(), "\n"; }
// EXPECT: 22 escaped=out20

echo "all-ok\n";
// EXPECT: all-ok
