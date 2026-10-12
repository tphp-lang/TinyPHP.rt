<?php
// EXPECT: int(1)
var_dump((int)1.9);
// EXPECT: int(-1)
var_dump((int)-1.9);
// EXPECT: bool(true)
var_dump((bool)1);
// EXPECT: bool(false)
var_dump((bool)"");
// EXPECT: bool(false)
var_dump((bool)"0");
// EXPECT: bool(true)
var_dump((bool)"0.0");
// EXPECT: bool(false)
var_dump((bool)[]);
// EXPECT: bool(true)
var_dump((bool)[0]);
// EXPECT: int(0)
var_dump((int)false);
// EXPECT: int(1)
var_dump((int)true);
// EXPECT: float(2)
var_dump((float)"2");
// EXPECT: string(4) "12.5"
var_dump((string)12.5);
// EXPECT: int(5)
var_dump((int)"5");
// EXPECT: string(1) "1"
var_dump((string)true);
// EXPECT: string(0) ""
var_dump((string)false);
// EXPECT: array(1) {
// EXPECT:   [0]=>
// EXPECT:   int(5)
// EXPECT: }
var_dump((array)5);
// EXPECT: array(0) {
// EXPECT: }
var_dump((array)null);
// EXPECT: string(1) "a"
var_dump((string)"a");
// EXPECT: int(3)
var_dump((int)"3" + 0);
// EXPECT: float(1.5)
var_dump((float)"1.5xyz");
