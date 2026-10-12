<?php
// EXPECT: 3
echo 1 + 2, "\n";
// EXPECT: -5
echo 2 - 7, "\n";
// EXPECT: 6
echo 2 * 3, "\n";
// EXPECT: 2.5
echo 5 / 2, "\n";
// EXPECT: 2
echo intdiv(7, 3), "\n";
// EXPECT: 1
echo 7 % 3, "\n";
// EXPECT: -1
echo -7 % 3, "\n";
// EXPECT: 8
echo 2 ** 3, "\n";
// EXPECT: 9223372036854775807
echo 9223372036854775806 + 1, "\n";
// EXPECT: 9.2233720368548E+18
echo 9223372036854775807 + 1, "\n";
// EXPECT: 0.3
echo 0.1 + 0.2, "\n";
// EXPECT: 10
echo 2 + 3 * 4 - 4, "\n";
// EXPECT: int(1)
var_dump(10 % 3 % 2);
// EXPECT: float(6)
var_dump(2.5 + 3.5);
// EXPECT: float(6)
var_dump(2.0 + 4);
// EXPECT: float(0.5)
var_dump(1 / 2);
// EXPECT: int(2)
var_dump(16 / 4 / 2);
