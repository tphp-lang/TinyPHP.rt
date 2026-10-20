<?php
// EXPECT-EXIT: 255
// final get 钩子不得被子类覆盖
class Base {
    public string $v {
        final get => "base";
    }
}
class Child extends Base {
    public string $v {
        get => "child";
    }
}
