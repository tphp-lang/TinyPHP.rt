<?php
// EXPECT: 7
echo 1 + 2 * 3, "\n";
// EXPECT: 9
echo (1 + 2) * 3, "\n";
// EXPECT: 512
echo 2 ** 3 ** 2, "\n";
// EXPECT: 2
echo 2 ** 1 ** 2, "\n";
// EXPECT: int(-9)
var_dump(-3 ** 2);
// EXPECT: int(9)
var_dump((-3) ** 2);
// EXPECT: ab12cd
echo "ab" . 1 . 2 . "cd", "\n";
// EXPECT: bool(true)
var_dump(1 + 1 == 2);
// EXPECT: bool(true)
var_dump(3 > 2 && 2 > 1);
// EXPECT: bool(true)
var_dump(1 || 0 && 0);
// EXPECT: bool(false)
var_dump(0 || 0 && 1);
// EXPECT: 6
$a = 0;
$a += 2 * 3;
echo $a, "\n";
// EXPECT: int(6)
var_dump(4 | 2 & 1 + 1);
// EXPECT: int(6)
var_dump(1 + 2 << 1);
// EXPECT: int(1)
var_dump(1 << 2 >> 2);
// EXPECT: int(1)
var_dump(10 % 3 % 2);
// EXPECT: int(2)
var_dump(16 / 4 / 2);
// EXPECT: bool(true)
var_dump((1 <=> 2) == -1);
// EXPECT: int(3)
var_dump(1 ^ 2);
// EXPECT: bool(true)
var_dump(!true == false);
// EXPECT: bool(true)
var_dump((int)"3" + 0 === 3);
// EXPECT: string(4) "mid1"
var_dump(false ? "a" : (true ? "mid1" : "mid2"));
$b = 0;
$r = $b ? 1 : 2;
// EXPECT: int(2)
var_dump($r);
// EXPECT: bool(true)
var_dump("a" . 1 == "a1");
// EXPECT: int(7)
var_dump(1 + (2 + 4));
// EXPECT: int(2)
var_dump(1 + 2 - 1 * 1);
// EXPECT: bool(true)
var_dump(1 < 2 == true);
// EXPECT: xyz
echo "x" . "y" . "z", "\n";
// EXPECT: int(1)
var_dump(print "");
// EXPECT: bool(true)
var_dump(true && true || false);
// EXPECT: bool(true)
var_dump(false || true && true);
// EXPECT: int(11)
var_dump(1 + 2 * 5);
// EXPECT: int(10)
var_dump((1 + 2) * 3 + 1);
// EXPECT: bool(true)
var_dump(5 % 3 == 2 && 1 == true);
