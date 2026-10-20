<?php
// EXPECT: prop=name
// EXPECT: ok

// PHP 8.4 __PROPERTY__ 魔术常量（钩子体内折为属性名）
class H {
    public string $name {
        get => "prop=" . __PROPERTY__;
    }
}
$h = new H();
echo $h->name, "\n";
echo "ok\n";
