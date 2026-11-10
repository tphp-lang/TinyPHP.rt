<?php
// minimal object loop for JIT coverage inspection
class M1 {
    public $v;
    function __construct($v) { $this->v = $v; }
    function get() { return $this->v; }
    function __destruct() { $this->v = -1; }
}
$s = 0;
for ($i = 0; $i < 100000; $i++) {
    $o = new M1($i);
    $s += $o->get();
    unset($o);
}
echo $s, "\n";
