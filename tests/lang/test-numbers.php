<?php
// EXPECT: 1
echo 1.0, "\n";
// EXPECT: 0.5
echo 0.5, "\n";
// EXPECT: 1.0E+15
echo 1.0e15, "\n";
// EXPECT: 1.0E+100
echo 1.0e100, "\n";
// EXPECT: 1.5
echo 1.5e0, "\n";
// EXPECT: 70000
echo 7.0E4, "\n";
// EXPECT: 123456789012345
echo 123456789012345, "\n";
// EXPECT: 1.2345678901235E+14
echo 123456789012345.678, "\n";
// EXPECT: -0.25
echo -0.25, "\n";
// EXPECT: 0.0001
echo 0.0001, "\n";
// EXPECT: 9.0E-5
echo 0.00009, "\n";
// EXPECT: float(0)
var_dump(0.0);
// EXPECT: float(1)
var_dump((float)1);
