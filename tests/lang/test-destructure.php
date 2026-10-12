<?php
[$a, $b] = [1, 2];
// EXPECT: int(1)
var_dump($a);
// EXPECT: int(2)
var_dump($b);
list($c, $d) = [3, 4];
// EXPECT: int(4)
var_dump($d);
[$k, , $m] = [1, 2, 3];
// EXPECT: int(3)
var_dump($m);
["x" => $x, "y" => $y] = ["x" => 10, "y" => 20];
// EXPECT: int(10)
var_dump($x);
// EXPECT: int(20)
var_dump($y);
list("p" => $p) = ["p" => 7];
// EXPECT: int(7)
var_dump($p);
// null rhs -> everything null
[$nz] = null;
// EXPECT: NULL
var_dump($nz);
list($nz2) = null;
// EXPECT: NULL
var_dump($nz2);
// keys mixed
["a" => $ka, 0 => $k0] = ["a" => 5, 0 => 6];
// EXPECT: int(5)
var_dump($ka);
// EXPECT: int(6)
var_dump($k0);
// destructure from function call
function pair() { return [8, 9]; }
[$e, $f] = pair();
// EXPECT: int(9)
var_dump($f);
