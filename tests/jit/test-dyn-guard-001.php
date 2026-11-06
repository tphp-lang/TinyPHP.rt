<?php
// 刀10：动态调用兜底正确性收尾。
// P1-13：usort/array_map/array_filter/array_reduce 回调 guarded 化——
//   回调内部 try/catch 捕获自身 raise（修复前：longjmp 穿 builtin C 帧 →
//   主分发器内部分恢复 → 挂死，JIT=0 同挂；PHP 正常）。
// P1-14：op_do_fcall 分发保水位（replay 不再踩外层 arg 队列）。
// 五模逐字一致：默认 / TPHP_DYN_JIT=0 / TPHP_NEST_CALL=1 / TPHP_JIT=0 / PHP 8.5。

function raise_it($m) { throw new Exception($m); }

// ---- P1-13 四形态：回调内部 catch（修复前挂死） ----
$arr = [3, 1, 2];
usort($arr, function ($a, $b) {
    try { if ($a === 1) raise_it("in-cmp"); } catch (Exception $e) { }
    return $a - $b;
});
echo "1 usort-inner-catch=", implode(",", $arr), "\n";
// EXPECT: 1 usort-inner-catch=1,2,3

$m = array_map(function ($x) { try { if ($x === 2) raise_it("in-map"); } catch (Exception $e) { } return $x * 10; }, [1, 2, 3]);
echo "2 map-inner-catch=", implode(",", $m), "\n";
// EXPECT: 2 map-inner-catch=10,20,30

$f = array_filter([1, 2, 3], function ($x) { try { if ($x === 2) raise_it("in-fil"); } catch (Exception $e) { } return $x % 2 === 1; });
echo "3 filter-inner-catch=", count($f), "\n";
// EXPECT: 3 filter-inner-catch=2

$r = array_reduce([1, 2, 3], function ($c, $x) { try { if ($x === 2) raise_it("in-red"); } catch (Exception $e) { } return $c + $x; }, 0);
echo "4 reduce-inner-catch=$r\n";
// EXPECT: 4 reduce-inner-catch=6

// ---- 逃逸形态：回调 raise 上抛（比较器逃逸 = escaped 标志快速回卷 + 收尾重抛） ----
$bad = [3, 1, 2];
try { usort($bad, function ($a, $b) { if ($a === 1) raise_it("esc5"); return $a - $b; }); echo "5 MISS\n"; }
catch (Exception $e) { echo "5 usort-escape=", $e->getMessage(), " kept=", count($bad), "\n"; }
// EXPECT: 5 usort-escape=esc5 kept=3

$src = [1, 2, 3];
try { array_map(function ($x) { raise_it("esc6"); return $x; }, $src); echo "6 MISS\n"; }
catch (Exception $e) { echo "6 map-escape=", $e->getMessage(), " src=", count($src), "\n"; }
// EXPECT: 6 map-escape=esc6 src=3

try { array_reduce([1, 2], function ($c, $x) { raise_it("esc7"); return $c; }, 0); echo "7 MISS\n"; }
catch (Exception $e) { echo "7 reduce-escape=", $e->getMessage(), "\n"; }
// EXPECT: 7 reduce-escape=esc7

// ---- 混合形态：同回调先 catch 后正常返回，排序继续 ----
$w = ["bb", "a", "ccc", "dd"];
usort($w, function ($a, $b) {
    try { if ($a === "a") raise_it("mid8"); } catch (Exception $e) { }
    $d = strlen($a) - strlen($b);
    return $d ?: ($a < $b ? -1 : ($a > $b ? 1 : 0));   // strcmp 未实现（既有缺口），手写字节序等价（ASCII 下 < > 同序）
});
echo "8 mixed=", implode(",", $w), "\n";
// EXPECT: 8 mixed=a,bb,dd,ccc

// ---- P1-14 回归：replay + refcounted 实参 + clean deopt → 值无损 ----
class W10 { public $n = 0; public function bump($v) { $this->n += $v; return $this->n; } }
function g10($x) { return $x; }
function f10($arr) {
    $w = new W10();
    $w->bump(1, 2);          // 多实参 → SEND replay（修复前踩 $arr 队列槽）
    $x = g10($arr);          // 非 INT → f 干净 deopt → 解释器序起点恢复
    return count($arr);
}
$a = [7, 8];
for ($i = 0; $i < 130; $i++) { f10($a); }   // 暖机：f 获 jit_entry 后走分发路径
echo "9 deopt-value=", f10($a), " src=", count($a), "\n";
// EXPECT: 9 deopt-value=2 src=2

echo "all-ok\n";
// EXPECT: all-ok
