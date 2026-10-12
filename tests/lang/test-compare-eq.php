<?php
// EXPECT: bool(true)
var_dump(0 == false);
// EXPECT: bool(true)
var_dump(1 == true);
// EXPECT: bool(true)
var_dump("" == null);
// EXPECT: bool(true)
var_dump("1" == 1);
// EXPECT: bool(false)
var_dump("abc" == 0);
// EXPECT: bool(true)
var_dump("123" == 123);
// EXPECT: bool(true)
var_dump("1e2" == 100);
// EXPECT: bool(false)
var_dump("1e2" === 100);
// EXPECT: bool(true)
var_dump(null == false);
// EXPECT: bool(true)
var_dump(null == 0);
// EXPECT: bool(true)
var_dump(null == "");
// EXPECT: bool(false)
var_dump("" == 0);
// EXPECT: bool(true)
var_dump(0 == "0");
// EXPECT: bool(true)
var_dump("10" == "1e1");
// EXPECT: bool(true)
var_dump([] == []);
// EXPECT: bool(true)
var_dump([1, 2] == [1, 2]);
// EXPECT: bool(false)
var_dump([1, 2] == [2, 1]);
// EXPECT: bool(true)
var_dump(["a" => 1] == ["a" => 1]);
// EXPECT: bool(false)
var_dump(["a" => 1] == ["a" => 2]);
// EXPECT: bool(false)
var_dump([] == "");
// EXPECT: bool(true)
var_dump([1] == [1]);
// EXPECT: bool(false)
var_dump([1] == [1, 2]);
// EXPECT: bool(true)
var_dump("1.0" == 1);
// EXPECT: bool(true)
var_dump(1.0 == 1);
// EXPECT: bool(true)
var_dump("  12" == 12);
// EXPECT: bool(false)
var_dump("12 " == 12);
