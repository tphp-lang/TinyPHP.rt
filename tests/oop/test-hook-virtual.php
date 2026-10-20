<?php
// EXPECT: virtual-OK
// EXPECT: ok

// 虚拟属性：get 钩子未引用 $this->name → 无 backing；读走钩子
class V {
    public string $name {
        get => "virtual-OK";
    }
}
$v = new V();
echo $v->name, "\n";
echo "ok\n";
