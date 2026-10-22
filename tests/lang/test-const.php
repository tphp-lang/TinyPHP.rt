<?php
const FOO = 100;
const BAR = FOO + 1;
// EXPECT: int(100)
var_dump(FOO);
// EXPECT: int(101)
var_dump(BAR);
// EXPECT: int(9223372036854775807)
var_dump(PHP_INT_MAX);
// EXPECT: int(-9223372036854775808)
var_dump(PHP_INT_MIN);
// EXPECT: int(8)
var_dump(PHP_INT_SIZE);
// EXPECT: float(2.2204460492503E-16)
var_dump(PHP_FLOAT_EPSILON);
// EXPECT: float(INF)
var_dump(INF);
// EXPECT: float(NAN)
var_dump(NAN);
// const in function body scope is still global
function readfoo() { return FOO; }
// EXPECT: int(100)
var_dump(readfoo());
// string value const
const GREETING = "hi";
// EXPECT: hi there
echo GREETING . " there", "\n";
// negative const via unary minus
const NEG = -5;
// EXPECT: int(-5)
var_dump(NEG);
// DIRECTORY_SEPARATOR / PATH_SEPARATOR：平台相关，断言形态而非字面值
// EXPECT: ds-ok
echo strlen(DIRECTORY_SEPARATOR) === 1
    && (DIRECTORY_SEPARATOR === "\\" || DIRECTORY_SEPARATOR === "/")
    ? "ds-ok\n" : "ds-FAIL\n";
// EXPECT: ps-ok
echo strlen(PATH_SEPARATOR) === 1
    && (PATH_SEPARATOR === ";" || PATH_SEPARATOR === ":")
    ? "ps-ok\n" : "ps-FAIL\n";
