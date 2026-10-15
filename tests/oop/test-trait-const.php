<?php
// P2-B: trait constants are imported into the using class (existing entry
// wins over a trait constant; PHP would fatal on a real collision)
trait HasConst {
    public const FROM_TRAIT = 't';
    public const SHARED = 'trait-value';
    public const NUM = 42;
}
trait HasConst2 {
    public const SECOND = 't2';
}
class Uses {
    use HasConst;
    public const OWN = 'own';
    public const SHARED = 'class-value';   // existing entry wins
}
class UsesBoth {
    use HasConst;
    use HasConst2;
}
var_dump(Uses::FROM_TRAIT);
var_dump(Uses::NUM);
var_dump(Uses::OWN);
var_dump(Uses::SHARED);
var_dump(UsesBoth::SECOND);
var_dump(UsesBoth::FROM_TRAIT);
var_dump(UsesBoth::SHARED);

// EXPECT: string(1) "t"
// EXPECT: int(42)
// EXPECT: string(3) "own"
// EXPECT: string(11) "class-value"
// EXPECT: string(2) "t2"
// EXPECT: string(1) "t"
// EXPECT: string(11) "trait-value"
