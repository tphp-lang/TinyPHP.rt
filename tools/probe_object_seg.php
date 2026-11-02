<?php
// P0-17 object component probe (post P0-20/ic/_setjmp era, 11-02).
// Segments: bare alloc+dtor / ctor call / method call / unset-dtor —
// each isolated in its own loop. Same shapes as the 10-31 four-segment
// split, re-measured after P0-20 pool + NEW_USER ic + MCALL snapshot +
// _setjmp.

class P0 {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function get() { return $this->v; }
    public function __destruct() { $this->v = -1; }
}

$n1 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) {
    $o = new P0($i);
    $n1 += $o->v;
}
$t1 = microtime(true);
echo "SEG ctor+prop      " . round(($t1 - $t0) * 1000, 1) . " ms\n";

$n2 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) {
    $o = new P0($i);
    $n2 += $o->get();
}
$t1 = microtime(true);
echo "SEG ctor+get       " . round(($t1 - $t0) * 1000, 1) . " ms\n";

$n3 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) {
    $o = new P0($i);
    unset($o);
    $n3++;
}
$t1 = microtime(true);
echo "SEG ctor+unset     " . round(($t1 - $t0) * 1000, 1) . " ms\n";

$n4 = 0;
$t0 = microtime(true);
for ($i = 0; $i < 100000; $i++) {
    $o = new class($i) {
        public $v;
        public function __construct($v) { $this->v = $v; }
        public function get() { return $this->v; }
        public function __destruct() { $this->v = -1; }
    };
    $n4 += $o->get();
}
$t1 = microtime(true);
echo "SEG anon-full      " . round(($t1 - $t0) * 1000, 1) . " ms\n";

echo "check=$n1/$n2/$n3/$n4\n";
