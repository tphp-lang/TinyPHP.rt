<?php
// EXPECT: new
// EXPECT: keep
// EXPECT: ok

// PHP 8.4 clone 属性列表：clone($o, ['p'=>v])
class C {
    public string $a = "old";
    public string $b = "keep";
}
$c = new C();
$d = clone($c, ["a" => "new"]);
echo $d->a, "\n";
echo $d->b, "\n";
echo "ok\n";
