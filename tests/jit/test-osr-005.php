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
