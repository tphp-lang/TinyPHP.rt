<?php
// EXPECT: base-get+child
// EXPECT: ok

// PHP 8.4 parent::$p::get()
class Base {
    public string $val {
        get => "base-get";
    }
}
class Child extends Base {
    public string $val {
        get => parent::$val::get() . "+child";
    }
}
$c = new Child();
echo $c->val, "\n";
echo "ok\n";
