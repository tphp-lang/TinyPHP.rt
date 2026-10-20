<?php
// EXPECT: INIT
// EXPECT: STORED
// EXPECT: ok

// backed：get 引用 $this->x → 有 backing；写入落 backing（无 set 钩子时直写）
class B {
    public string $x = "init" {
        get => strtoupper($this->x);
    }
}
$b = new B();
echo $b->x, "\n";
$b->x = "stored";
echo $b->x, "\n";
echo "ok\n";
