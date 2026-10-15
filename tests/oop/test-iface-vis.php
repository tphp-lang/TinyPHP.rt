<?php
// EXPECT-EXIT: 255
// P3: 接口方法必须 public（PHP: Access type for interface method ... must be public）
interface Bad {
    protected function f();
}
