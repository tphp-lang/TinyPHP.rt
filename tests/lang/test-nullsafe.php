<?php
// P2-A: nullsafe operator ?-> — PHP semantics: any null hop short-circuits
// the WHOLE chain to null (later plain -> hops are implicitly guarded)
class Box {
    public ?Box $next = null;
    public int $v = 7;
    public function half(): int { return 3; }
    public function mark(): Box { echo "marked\n"; return $this; }
}

$a = new Box();
var_dump($a?->v);
$nil = null;
var_dump($nil?->v);
var_dump($a?->next->v);
$b = new Box();
$b->next = new Box();
var_dump($b?->next->v);
var_dump($a?->half());
$nil2 = null;
var_dump($nil2?->half());
$a2 = new Box();
var_dump($a2?->mark()->half());
$nil3 = null;
var_dump($nil3?->mark()->half());
var_dump($a?->next->half());

// EXPECT: int(7)
// EXPECT: NULL
// EXPECT: NULL
// EXPECT: int(7)
// EXPECT: int(3)
// EXPECT: NULL
// EXPECT: marked
// EXPECT: int(3)
// EXPECT: NULL
// EXPECT: NULL
