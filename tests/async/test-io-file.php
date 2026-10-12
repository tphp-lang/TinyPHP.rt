<?php
// M4-b: 透明文件 I/O —— put/get、fopen/fwrite/fgets/fseek/fread/feof/fclose、
// 追加模式、exists/rename/unlink、失败路径（warning + false）
// 用相对路径 _io_t/，脚本末尾自清理。
mkdir("_io_t");

// 1) file_put_contents / file_get_contents
var_dump(file_put_contents("_io_t/a.txt", "line1\nline2\nline3"));
var_dump(file_get_contents("_io_t/a.txt"));

// 2) fopen/fwrite + fgets 序列
$f = fopen("_io_t/b.txt", "w");
var_dump($f instanceof TStream);
fwrite($f, "alpha\nbeta\ngamma");
fclose($f);

$f = fopen("_io_t/b.txt", "r");
var_dump(fgets($f));
var_dump(fgets($f));
var_dump(fgets($f));
var_dump(fgets($f));            // EOF → false
var_dump(feof($f));
fclose($f);

// 3) fseek（绝对 / SEEK_END 相对）+ fread
$f = fopen("_io_t/b.txt", "r");
fseek($f, 6);
var_dump(fread($f, 4));         // "beta"
fseek($f, -5, SEEK_END);
var_dump(fread($f, 5));         // "gamma"
var_dump(fread($f, 8));         // EOF → ""
fclose($f);

// 4) 追加模式 + filesize
$f = fopen("_io_t/b.txt", "a");
fwrite($f, "+more");
fclose($f);
var_dump(filesize("_io_t/b.txt"));
var_dump(file_get_contents("_io_t/b.txt"));

// 5) file_exists / rename / unlink
var_dump(file_exists("_io_t/b.txt"));
var_dump(rename("_io_t/b.txt", "_io_t/c.txt"));
var_dump(file_exists("_io_t/b.txt"));
var_dump(unlink("_io_t/c.txt"));
var_dump(file_exists("_io_t/c.txt"));

// 6) 失败路径（PHP Warning 到 stderr；stdout 只见 false）
var_dump(file_get_contents("_io_t/nope.txt"));
var_dump(fopen("_io_t/nope.txt", "r"));

unlink("_io_t/a.txt");
rmdir("_io_t");
echo "cleanup\n";

// EXPECT: int(17)
// EXPECT: string(17) "line1
// EXPECT: line2
// EXPECT: line3"
// EXPECT: bool(true)
// EXPECT: string(6) "alpha
// EXPECT: "
// EXPECT: string(5) "beta
// EXPECT: "
// EXPECT: string(5) "gamma"
// EXPECT: bool(false)
// EXPECT: bool(true)
// EXPECT: string(4) "beta"
// EXPECT: string(5) "gamma"
// EXPECT: string(0) ""
// EXPECT: int(21)
// EXPECT: string(21) "alpha
// EXPECT: beta
// EXPECT: gamma+more"
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(false)
// EXPECT: bool(true)
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: bool(false)
// EXPECT: cleanup
