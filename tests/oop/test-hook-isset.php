<?php
// EXPECT: bool(true)
// EXPECT: bool(false)
// EXPECT: ok

// isset 带 get 钩子 → 调钩子，非 null 为 true
class I {
    public string $y {
        get => "from-get";
    }
}
$i = new I();
var_dump(isset($i->y));
var_dump(isset($i->nope));
echo "ok\n";
