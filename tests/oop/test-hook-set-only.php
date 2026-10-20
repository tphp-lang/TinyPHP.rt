<?php
// EXPECT: hello
// EXPECT: ok

// set-only 虚拟属性：写走 set 钩子（隐式 $value）
class SO {
    public string $saved = "";
    public string $alias {
        set { $this->saved = $value; }
    }
}
$o = new SO();
$o->alias = "hello";
echo $o->saved, "\n";
echo "ok\n";
