<?php
// function-level INIT_MCALL/NEW_USER fold probe (batch 2-7).
// ~main loops are OSR chunks (batch 2-4/2-5 already fold there); these
// driver METHODS/functions compile at the op_do_fcall eager/hot gate =
// function-level units where the fold was previously unavailable
// (j->osr gate). Receiver rides $this->b (property fetch pad is a
// constant cost) so no object is passed as a param -- JIT->JIT static
// calls enforce the INT param contract and would deopt the chunk.
class PrB { public function id($x) { return $x + 1; } }
class PrPlain { public $v; }
class PrC { public $v; public function __construct($v) { $this->v = $v; } }
class PrDrv {
    public $b;
    public function __construct() { $this->b = new PrB(); }
    public function step($x) { return $this->b->id($x); }   // fn-level mcall
}
function wf_new($i) { $q = new PrPlain(); $q->v = $i; return $q->v; }   // fn-level no-ctor new
function wf_ctor($i) { $q = new PrC($i); return $q->v; }                // batch 2-8: user-ctor fold 直调（对照 fold 前 BASE）

$w = new PrDrv();
$s = 0;
$t0 = microtime(true);
for ($i = 0; $i < 2000000; $i++) $s = $w->step($s);
$t1 = microtime(true);
echo "SEG fn-mcall  " . round(($t1 - $t0) * 1000, 1) . " ms\n";
$p = 0;
$t0 = microtime(true);
for ($i = 0; $i < 2000000; $i++) $p += wf_new($i);
$t1 = microtime(true);
echo "SEG fn-new    " . round(($t1 - $t0) * 1000, 1) . " ms\n";
$c = 0;
$t0 = microtime(true);
for ($i = 0; $i < 2000000; $i++) $c += wf_ctor($i);
$t1 = microtime(true);
echo "SEG fn-ctor   " . round(($t1 - $t0) * 1000, 1) . " ms\n";
echo "check=$s/$p/$c\n";
