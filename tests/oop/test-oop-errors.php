<?php
// OOP 错误场景：实例化/契约/继承限制（每段独立 fatal）
// EXPECT-EXIT: 255

// --- 1. 实例化抽象类 ---
abstract class AbsBase {
    abstract public function go(): string;
}
new AbsBase();
