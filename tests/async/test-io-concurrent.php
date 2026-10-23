<?php
// M4-b: 协程内并发文件 I/O —— file_get_contents 挂起期间其他协程推进；
// 大内容读写（跨多块）；scandir 含 "." ".."
use function Tphp\spawn;
use function Tphp\await;

mkdir("_io_t");

// 两个文件：并发读（c2 无 I/O 直接返回——c1 挂起期间 c2 完成）
file_put_contents("_io_t/f1.txt", "AAAA");
file_put_contents("_io_t/f2.txt", "BBBBBB");

$c1 = spawn(function () {
    return file_get_contents("_io_t/f1.txt");
});
$c2 = spawn(function () {
    return "c2-no-block";
});
var_dump(await($c1));
var_dump(await($c2));

// 读第二个文件 + 返回拼接（挂起恢复后继续跑）
$c3 = spawn(function () {
    $a = file_get_contents("_io_t/f1.txt");
    $b = file_get_contents("_io_t/f2.txt");
    return $a . $b;
});
var_dump(await($c3));

// 大内容（512KB，跨多块读）
$big = str_repeat("x", 512 * 1024 - 1) . "!";
file_put_contents("_io_t/big.txt", $big);
var_dump(file_get_contents("_io_t/big.txt") === $big);

// scandir 条目
$names = scandir("_io_t");
var_dump(in_array("f1.txt", $names), in_array("f2.txt", $names),
         in_array("big.txt", $names));
var_dump(in_array(".", $names), in_array("..", $names));

unlink("_io_t/f1.txt");
unlink("_io_t/f2.txt");
unlink("_io_t/big.txt");
rmdir("_io_t");
echo "cleanup\n";

// EXPECT: string(4) "AAAA"
// EXPECT: string(11) "c2-no-block"
// EXPECT: string(10) "AAAABBBBBB"
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: cleanup
