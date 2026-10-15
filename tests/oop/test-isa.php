<?php
// P3: is_a / is_subclass_of / get_parent_class
class A {}
class B extends A {}
interface I {}
class C implements I {}

$b = new B();
var_dump(is_a($b, A::class));
var_dump(is_a($b, B::class));          // includes self
var_dump(is_a($b, 'A'));               // string class arg
var_dump(is_a('B', 'A'));              // allow_string default true
var_dump(is_a($b, 'A', false) === false ? 'no-obj-false' : 'no-obj-wrong');  // object: unaffected
var_dump(is_a('B', 'A', false) === false ? 'str-false' : 'str-wrong');       // string: disallowed
var_dump(is_subclass_of($b, A::class));
var_dump(is_subclass_of($b, B::class)); // excludes self
var_dump(is_subclass_of('C', I::class)); // interface + string
var_dump(get_parent_class($b));
var_dump(get_parent_class('B'));
var_dump(get_parent_class('A'));        // no parent -> false
var_dump(is_a(new C(), I::class));

// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: string(12) "no-obj-wrong"
// EXPECT: string(9) "str-false"
// EXPECT: bool(true)
// EXPECT: bool(false)
// EXPECT: bool(true)
// EXPECT: string(1) "A"
// EXPECT: string(1) "A"
// EXPECT: bool(false)
// EXPECT: bool(true)
