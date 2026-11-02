<?php
// P1-12 batch 2-3 re-verification (2026-11-02, post-f97555a session):
// INIT_MCALL / INIT_NEW / NEW_USER whitelisted for OSR chunks again.
// 10-31 first attempt was reverted: O1+FOLDING CSE produced bad code on
// LOOP x opseq-replay-MERGE (tv_copy src=0, deps/ir domain). The kv
// thread's identical "blocked upstream" attribution was later disproven
// by f97555a -- this file replays the shapes from that attempt:
//   c1 = method-call loop        (ran correctly even in the 10-31 attempt)
//   c2 = new + prop + COPY loop  (the segfault shape)
// TPHP_JIT=0/1 must produce identical output. Main stays try-free.
// EXPECT: c1=255000
// EXPECT: c2=135000
// EXPECT: c3=135000
// EXPECT: c4=270000
// EXPECT: c5=39999
// EXPECT: c6=449985000

// 1) method call inside the loop (INIT_MCALL fold -> replay/direct chain)
class OsrM {
    public function add1($x) { return $x + 1; }
}
$om = new OsrM();
$s1 = 0;
for ($i = 0; $i < 30000; $i++) {
    $s1 += $om->add1($i & 15);
}
echo "c1=$s1\n";

// 2) object chain: new + prop read/write + COPY inside the loop
//    (the 10-31 crash shape: slot-address CSE across MERGE branches)
class OsrBox2 {
    public $v;
    public function __construct($v) { $this->v = $v; }
}
$s2 = 0;
for ($i = 0; $i < 30000; $i++) {
    $o = new OsrBox2($i & 7);
    $o->v = $o->v + 1;
    $t = $o->v;
    $u = $t;
    $s2 += $u;
}
echo "c2=$s2\n";

// 3) combined shape: new + method call + prop + COPY in one loop
//    (maximises the LOOP x opseq-replay-MERGE interplay of the 10-31 crash)
class OsrC3 {
    public $v;
    public function __construct($v) { $this->v = $v; }
    public function bump() { $this->v = $this->v + 1; return $this->v; }
}
$s3 = 0;
for ($i = 0; $i < 30000; $i++) {
    $o = new OsrC3($i & 7);
    $u = $o->bump();
    $s3 += $u;
}
echo "c3=$s3\n";

// 4) batch 2-4 fold: frameless method returning $this exercises the
//    $this addref/release balance (callee moves the ref into ret);
//    string-arg method trips the INT contract -> seq re-run (correctness)
class OsrR {
    public $v = 7;
    public function me() { return $this; }
    public function tag($s) { return $s; }
}
$rr = new OsrR();
$s4 = 0;
for ($i = 0; $i < 30000; $i++) {
    $x = $rr->me();
    $s4 += $x->v;
    $t = $rr->tag("ab");   /* standalone seq (nested-in-arg calls predate
                            * this batch: INIT_FCALL precedes arg eval) */
    $s4 += strlen($t);
}
echo "c4=$s4\n";

// 5) polymorphic receiver: ic snapshot stays OsrA; after the mid-loop
//    switch the cls guard fails every iteration -> interpreter re-run
//    from INIT_MCALL (20001 x 1 + 9999 x 2)
class OsrA { public function w() { return 1; } }
class OsrB { public function w() { return 2; } }
$oa = new OsrA();
$ob = new OsrB();
$pk = $oa;
$s5 = 0;
for ($i = 0; $i < 30000; $i++) {
    $s5 += $pk->w();
    if ($i == 20000) { $pk = $ob; }
}
echo "c5=$s5\n";

// 6) batch 2-5 fold: no-ctor class -> alloc + dst landing direct in the
//    chunk (old dst released after alloc, interpreter-identical rc net)
class OsrPlain { public $v; }
$sp = 0;
for ($i = 0; $i < 30000; $i++) {
    $q = new OsrPlain();
    $q->v = $i;
    $sp += $q->v;
}
echo "c6=$sp\n";
