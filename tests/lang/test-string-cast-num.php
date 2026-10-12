<?php
// EXPECT: 124
echo "123abc" + 1, "\n";
// EXPECT: 1
echo "1" + 0, "\n";
// EXPECT: 7
echo " 7 " + 0, "\n";
// EXPECT: 6
echo "3.5" + 2.5, "\n";
// EXPECT: bool(true)
var_dump(is_numeric("12.5"));
// EXPECT: bool(false)
var_dump(is_numeric("0x1A"));
// EXPECT: bool(false)
var_dump(is_numeric("  42  "));
// EXPECT: bool(false)
var_dump(is_numeric("42 "));
// EXPECT: bool(true)
var_dump(is_numeric("1e3"));
// EXPECT: bool(true)
var_dump(is_numeric("-5"));
// EXPECT: bool(false)
var_dump(is_numeric(""));
// EXPECT: bool(false)
var_dump(is_numeric("12a"));
// EXPECT: int(42)
var_dump((int)"42abc");
// EXPECT: int(0)
var_dump((int)"abc");
// EXPECT: int(0)
var_dump((int)"0x0F");
// EXPECT: int(8)
var_dump((int)"08");
// EXPECT: float(1.5)
var_dump((float)"1.5xyz");
// EXPECT: int(5)
var_dump((int)"  5  ");
