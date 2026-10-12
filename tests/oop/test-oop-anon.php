<?php
// 匿名类：extends + implements + 构造器参数 + 方法调用
// EXPECT: hi:anon+base
// EXPECT: hi:second
// EXPECT: twice

interface Greet {
    public function hi(): string;
}

class Base {
    public function who() { return "base"; }
}

$obj = new class("anon") extends Base implements Greet {
    public function __construct(private string $tag) {}
    public function hi(): string { return "hi:{$this->tag}+{$this->who()}"; }
};
echo $obj->hi(), "\n";

// 第二个匿名类：同文件不同声明点各自唯一
$obj2 = new class("second") implements Greet {
    public function hi(): string { return "hi:second"; }
};
echo $obj2->hi(), "\n";

// 匿名类实现接口契约（缺方法会加载期 fatal，能实例化说明契约满足）
$cb = new class implements Greet {
    public function hi(): string { return "twice"; }
};
echo $cb->hi(), "\n";
