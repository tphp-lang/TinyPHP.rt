<?php
// EXPECT: 7
// EXPECT: ok

// PHP 8.5 final 提升属性
class P {
    public function __construct(final private int $x) {}
    public function getX(): int { return $this->x; }
}
$p = new P(7);
echo $p->getX(), "\n";
echo "ok\n";
