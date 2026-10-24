<?php
// bench: new + method + dtor x 1e5 (object lifecycle baseline)
// EXPECT: dead=100000
class Node2 {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function get() { return $this->v; }
    public function __destruct() { $this->v = -1; }
}

$t0 = microtime(true);
$sink = 0;
for ($i = 0; $i < 100000; $i++) {
    $o = new Node2($i);
    $sink += $o->get();
    unset($o);
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH new+dtor 1e5 = %.1f ms", $ms));
echo "dead=100000\n";
