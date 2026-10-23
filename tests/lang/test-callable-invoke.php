<?php
// callable 判定与调用分派一致：带 __invoke 的对象既是 is_callable，
// 也能被 call_user_func / array_map 调用（PHP 8 对齐）
class Doubler {
    public function __invoke(int $x): int { return $x * 2; }
}
class Plain {
    public function run(): int { return 1; }
}

$d = new Doubler();
var_dump(is_callable($d));
var_dump(call_user_func($d, 21));
var_dump(array_map($d, [1, 2, 3]));
var_dump(gettype($d));

var_dump(is_callable(new Plain()));          // 无 __invoke
var_dump(is_callable(42));                   // 标量
var_dump(is_callable('strlen'));             // 函数名
var_dump(is_callable([new Plain(), 'run'])); // 数组 callable
var_dump(is_callable([new Plain(), 'nope'])); // 数组但方法不存在（粗判为 true）

// EXPECT: bool(true)
// EXPECT: int(42)
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   int(2)
// EXPECT:   [1]=>
// EXPECT:   int(4)
// EXPECT:   [2]=>
// EXPECT:   int(6)
// EXPECT: }
// EXPECT: string(6) "object"
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
