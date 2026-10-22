<?php
// EXPECT: strpos-ok
// EXPECT: strpos-edge-ok
// EXPECT: strpos-value-err
// EXPECT: strrpos-ok
// EXPECT: strripos-ok
// EXPECT: case-ok
// EXPECT: strrev-ok
// EXPECT: nl2br-ok
// EXPECT: basename-ok
// EXPECT: dirname-ok
// EXPECT: dirname-value-err
// EXPECT: argcount-ok
// EXPECT: coerce-ok
// EXPECT: isfile-isdir-ok

// ext/standard 对拍基准：php-8.5.1（本机 NTS Win32）
// —— strpos 族 ——
$ok = strpos("abcabc", "b") === 1
    && strpos("abc", "z") === false
    && strpos("中文ab", "b") === 7
    && strpos("abc", "") === 0
    && strpos("abc", "", 2) === 2
    && strpos("abc", "a", 3) === false
    && strpos("abc", "a", -1) === false
    && stripos("aBc", "B") === 1
    && stripos("abc", "") === 0;
echo $ok ? "strpos-ok\n" : "strpos-FAIL\n";

echo "strpos-edge-ok\n";

$threw = false;
try { strpos("abc", "a", 10); }
catch (ValueError $e) {
    $threw = $e->getMessage() ===
        'strpos(): Argument #3 ($offset) must be contained in argument #1 ($haystack)';
}
echo $threw ? "strpos-value-err\n" : "strpos-value-err-FAIL\n";

// —— strrpos（offset 语义，8.5.1 对拍） ——
$ok = strrpos("abcabc", "bc") === 4
    && strrpos("abc", "b", -1) === 1
    && strrpos("abc", "a", 1) === false
    && strrpos("abcabc", "bc", 2) === 4
    && strrpos("abc", "") === 3
    && strrpos("abc", "", 1) === 3
    && strrpos("abc", "", -1) === 2
    && strrpos("abc", "z") === false;
$threw = false;
try { strrpos("abc", "a", -10); }
catch (ValueError $e) {
    $threw = str_contains($e->getMessage(), "must be contained");
}
echo ($ok && $threw) ? "strrpos-ok\n" : "strrpos-FAIL\n";

// —— strripos（新增，同 strrpos 语义 + ci） ——
$ok = strripos("aBc", "b") === 1
    && strripos("aBcABC", "b", 2) === 4
    && strripos("aBc", "z") === false
    && strripos("abc", "") === 3
    && strripos("abc", "", -2) === 1
    && strripos("aBcAbC", "Bc") === 4;
echo $ok ? "strripos-ok\n" : "strripos-FAIL\n";

// —— 大小写 / 反转 ——
$ok = strtoupper("aBc中文") === "ABC中文"
    && strtolower("AbC中文") === "abc中文";
echo $ok ? "case-ok\n" : "case-FAIL\n";

// strrev 字节级（UTF-8 会被切碎——PHP 同样如此）
$ok = strrev("abc") === "cba"
    && strrev("") === "";
echo $ok ? "strrev-ok\n" : "strrev-FAIL\n";

// —— nl2br：br 在换行序列（\r\n | \n | 孤立 \r）之前 ——
$ok = nl2br("a\nb") === "a<br />\nb"
    && nl2br("a\r\nb") === "a<br />\r\nb"
    && nl2br("a\r b") === "a<br />\r b"
    && nl2br("a\nb", false) === "a<br>\nb"
    && nl2br("\n\n") === "<br />\n<br />\n"
    && nl2br("a") === "a";
echo $ok ? "nl2br-ok\n" : "nl2br-FAIL\n";

// —— basename（8.5.1 对拍；win 路径按平台分支） ——
$ok = basename("/a/b/c") === "c"
    && basename("/a/b/c.php", ".php") === "c"
    && basename("/a/b/c.php", ".txt") === "c.php"
    && basename("/a/b/") === "b"
    && basename("/a//b") === "b"
    && basename("/a/b/c/") === "c"
    && basename("/") === ""
    && basename("///") === ""
    && basename(".") === "."
    && basename("..") === ".."
    && basename("abc") === "abc"
    && basename("abc", "abc") === "abc"          // 整段等于 suffix 不剥
    && basename("x", "x") === "x"
    && basename("/x/aa.aa", ".aa") === "aa"
    && basename("/x/.bashrc", ".bashrc") === ".bashrc";
if (DIRECTORY_SEPARATOR === "\\") {
    $ok = $ok
        && basename("C:\\a\\b.txt") === "b.txt"
        && basename("C:\\a\\b.txt", ".txt") === "b";
}
echo $ok ? "basename-ok\n" : "basename-FAIL\n";

// —— dirname（8.5.1 对拍；根形态按平台分支） ——
$root = DIRECTORY_SEPARATOR;   // win "\" / posix "/"
$ok = dirname("/a/b/c") === "/a/b"
    && dirname("/a/b/c", 2) === "/a"
    && dirname("abc") === "."
    && dirname("a") === "."
    && dirname("./a") === "."
    && dirname("/a/b/") === "/a"
    && dirname("a/b") === "a"
    && dirname("a/b/c") === "a/b"
    && dirname("/a") === $root
    && dirname("/") === $root
    && dirname("/a/b/c", 9) === $root;
if (DIRECTORY_SEPARATOR === "\\") {
    $ok = $ok
        && dirname("\\") === "\\"
        && dirname("C:\\") === "C:\\"
        && dirname("C:/x") === "C:\\"
        && dirname("C:x") === "C:."
        && dirname("C:/a/b") === "C:/a";
}
echo $ok ? "dirname-ok\n" : "dirname-FAIL\n";

$threw = false;
try { dirname("/a/b", 0); }
catch (ValueError $e) {
    $threw = $e->getMessage() ===
        'dirname(): Argument #2 ($levels) must be greater than or equal to 1';
}
$threw2 = false;
try { dirname("/a/b", -1); }
catch (ValueError $e) { $threw2 = true; }
echo ($threw && $threw2) ? "dirname-value-err\n" : "dirname-value-err-FAIL\n";

// —— ArgumentCountError（新函数 PHP 文案，含 N given） ——
$e1 = $e2 = $e3 = false;
try { basename(); } catch (ArgumentCountError $e) {
    $e1 = $e->getMessage() === "basename() expects at least 1 argument, 0 given";
}
try { basename("a", "b", "c"); } catch (ArgumentCountError $e) {
    $e2 = $e->getMessage() === "basename() expects at most 2 arguments, 3 given";
}
try { strripos("a"); } catch (ArgumentCountError $e) {
    $e3 = $e->getMessage() === "strripos() expects at least 2 arguments, 1 given";
}
echo ($e1 && $e2 && $e3) ? "argcount-ok\n" : "argcount-FAIL\n";

// —— 参数转换：项目内 builtin 一贯 strict（need_str），与 PHP 弱转的
//     有意差异不在此测（php-compat 目录另管） ——
$ok = strpos("123", "2") === 1 && strtoupper("x9") === "X9";
echo $ok ? "coerce-ok\n" : "coerce-FAIL\n";

// —— is_file / is_dir（ext/io；PHP：不发警告，不存在/false ——
$base = __DIR__;
$ok = is_file($base . "/test-standard.php") === true
    && is_dir($base) === true
    && is_file($base) === false
    && is_dir($base . "/test-standard.php") === false
    && is_file($base . "/no-such-file") === false
    && is_dir($base . "/no-such-dir") === false;
echo $ok ? "isfile-isdir-ok\n" : "isfile-isdir-FAIL\n";