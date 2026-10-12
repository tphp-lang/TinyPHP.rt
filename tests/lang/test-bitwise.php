<?php
// EXPECT: int(6)
var_dump(4 | 2);
// EXPECT: int(4)
var_dump(6 & 4);
// EXPECT: int(5)
var_dump(6 ^ 3);
// EXPECT: int(-16)
var_dump(~15);
// EXPECT: int(8)
var_dump(1 << 3);
// EXPECT: int(2)
var_dump(8 >> 2);
// EXPECT: int(-1)
var_dump(-1 >> 5);
// EXPECT: int(-1)
var_dump(-1 >> 1);
// EXPECT: int(-4)
var_dump(-8 >> 1);
// EXPECT: int(-1)
var_dump(1 << 63 >> 63);
// EXPECT: int(1)
var_dump(true & true);
// EXPECT: int(0)
var_dump(true ^ true);
// EXPECT: int(15)
var_dump(0xF);
// EXPECT: int(3)
var_dump(0b11);
// EXPECT: int(8)
var_dump(0o10);
// EXPECT: int(2)
var_dump(1_0 - 8);
// EXPECT: int(255)
var_dump(0xFF);
// EXPECT: int(1)
var_dump(1 & 3 | 0 ^ 0);
