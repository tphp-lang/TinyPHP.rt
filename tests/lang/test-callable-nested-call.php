<?php
// callable 调用路径（usort / call_user_func / array_map）的闭包内
// 再调用户函数：子帧必须在同一循环内跑完。
// 旧式 thunk 循环"帧切换即退出"会让子帧挂在栈上未执行 →
// 帧链错乱 + 调用结果没写进临时槽（read of undefined temporary）
function cmp(int $a, int $b): int { return $a <=> $b; }
function double(int $x): int { return $x * 2; }

// usort 回调内调用户函数
$arr = [3, 1, 2];
usort($arr, function ($a, $b) { return cmp($a, $b); });
echo "usort=", implode(",", $arr), "\n";

// call_user_func 回调内调用户函数
$r = call_user_func(function (int $x) { return double($x) + 1; }, 5);
echo "cuf=", $r, "\n";

// array_map 回调内调用户函数
$m = array_map(function (int $x) { return double($x); }, [1, 2, 3]);
echo "map=", implode(",", $m), "\n";

// 回调内调用户方法（方法帧路径）
class Calc {
    public function twice(int $x): int { return $x * 2; }
}
$c = new Calc();
$m2 = array_map(function (int $x) use ($c) { return $c->twice($x); }, [2, 4]);
echo "method=", implode(",", $m2), "\n";

// EXPECT: usort=1,2,3
// EXPECT: cuf=11
// EXPECT: map=2,4,6
// EXPECT: method=4,8
