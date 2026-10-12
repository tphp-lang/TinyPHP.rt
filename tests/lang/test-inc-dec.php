<?php
$a = 5;
// EXPECT: int(6)
var_dump(++$a);
// EXPECT: int(6)
var_dump($a);
$b = 5;
// EXPECT: int(5)
var_dump($b++);
// EXPECT: int(6)
var_dump($b);
$c = 5;
// EXPECT: int(4)
var_dump(--$c);
$d = 5;
// EXPECT: int(5)
var_dump($d--);
// EXPECT: int(4)
var_dump($d);
$n = null;
// EXPECT: int(1)
var_dump(++$n);
$n = null;
// EXPECT: NULL
var_dump(--$n);
$t = true;
// EXPECT: bool(true)
var_dump(++$t);
$t = true;
// EXPECT: bool(true)
var_dump(--$t);
$s = "a";
// EXPECT: string(1) "b"
var_dump(++$s);
$s = "z";
// EXPECT: string(2) "aa"
var_dump(++$s);
$s = "az";
// EXPECT: string(2) "ba"
var_dump(++$s);
$s = "zz";
// EXPECT: string(3) "aaa"
var_dump(++$s);
$s = "5";
// EXPECT: int(6)
var_dump(++$s);
$f = 1.5;
// EXPECT: float(2.5)
var_dump(++$f);
$arr = [10];
// EXPECT: int(11)
var_dump(++$arr[0]);
// EXPECT: int(11)
var_dump($arr[0]);
$arr2 = [7];
// EXPECT: int(7)
var_dump($arr2[0]++);
// EXPECT: int(8)
var_dump($arr2[0]);
$g = null;
// EXPECT: NULL
var_dump(--$g);
