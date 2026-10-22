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
// PHP_OS_FAMILY：平台相关家族名（形态断言，CI 多平台通用）
// EXPECT: osf-ok
echo in_array(PHP_OS_FAMILY, ["Windows", "Linux", "Darwin", "BSD"])
    ? "osf-ok\n" : "osf-FAIL\n";
// PHP_VERSION：非空 x.y.z 形态
// EXPECT: ver-ok
echo strlen(PHP_VERSION) >= 5 && strpos(PHP_VERSION, ".") !== false
    ? "ver-ok\n" : "ver-FAIL\n";
// PHP_PACK：测试以源码脚本运行（非打包 exe），恒为 false
// EXPECT: bool(false)
var_dump(PHP_PACK);
// PHP_UNAME：uname(2) 五字段关联数组（uv_os_uname），全字符串值
// EXPECT: uname-ok
$uk = ["sysname", "nodename", "release", "version", "machine"];
$uok = is_array(PHP_UNAME) && count(PHP_UNAME) === 5;
foreach ($uk as $k) {
    if (!isset(PHP_UNAME[$k]) || !is_string(PHP_UNAME[$k])
        || PHP_UNAME[$k] === "") $uok = false;
}
echo $uok ? "uname-ok\n" : "uname-FAIL\n";
