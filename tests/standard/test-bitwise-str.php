<?php
// EXPECT: b01=str 1 61
// EXPECT: b02=str 3 20 20 20
// EXPECT: b03=str 2 9e 9d
// EXPECT: b04=int 7
// EXPECT: b05=str 1 03
// EXPECT: b06=str 2 61 62
// EXPECT: b07=str 3 61 62 11
// EXPECT: b08=str 2 f0 f0
// EXPECT: b09=str 2 20 20
// EXPECT: b10=str 1 10
// EXPECT: b11=str 0
// EXPECT: b12=str 0
// EXPECT: b13=str 2 61 62
// EXPECT: b14=str 1 ca
// EXPECT: b15=str 2 00 41
// EXPECT: b16=str 1 10
// EXPECT: b17=str 5 68 65 6c 6c 6f
// EXPECT: b18=str 6 73 74 72 69 6e 67
// EXPECT: b19=str 7 69 6e 74 65 67 65 72
// EXPECT: b20=str 2 00 00
// EXPECT: b21=str 1 61
// EXPECT: b22=str 3 61 00 63

// 2026-11-14 bug 修复回归：位运算（^ & | ~）两操作数均为 string 时逐字节
// 返回 string（修复前 tval_to_int 收缩成 int，恒 0/-1 静默错）。语义对齐
// PHP zend_operators.c bitwise_string_function 族，EXPECT 值三方实测取自
// tphp（JIT=1/JIT=0）与 PHP 8.5.1 逐字节一致的探针输出：& 与 ^ 结果长
// MIN（截断），| 长 MAX（短侧 \0 补齐），~ 逐字节取反；混合 string/int
// 仍走整数路径（b04）；二进制安全（串内 \0，b15/b22）。
function rep($tag, $v) {
    if (is_int($v)) { echo $tag, "=int ", $v, "\n"; return; }
    $hx = "0123456789abcdef";
    echo $tag, "=str ", strlen($v);
    for ($i = 0; $i < strlen($v); $i++) {
        $c = ord($v[$i]);
        echo " ", $hx[(int)($c / 16)], $hx[$c % 16];
    }
    echo "\n";
}
rep("b01", "p" ^ "\x11");            // 报告首行：修复前 int 0
rep("b02", "abc" ^ "ABC");           // 修复前 int 0
rep("b03", ~"ab");                   // 修复前 int -1
rep("b04", "5" ^ 2);                 // 混合 string/int 保持整数路径
rep("b05", "1" ^ "2");               // 数字串也走字符串路径（修复前 int 3）
rep("b06", "abcd" & "ab");           // & MIN 截断
rep("b07", "ab" | "\x00\x00\x11");   // | MAX 补齐
rep("b08", "\xff\x0f" ^ "\x0f\xff"); // 高位字节
$s9 = "ab"; $s9 ^= "AB";             // 复合赋值同层
rep("b09", $s9);
$s10 = "p"; $s10 &= "\x11";
rep("b10", $s10);
rep("b11", "" ^ "ab");               // ^ MIN：空串 → 空
rep("b12", "" & "ab");
rep("b13", "" | "ab");
rep("b14", ~"5");                    // 0x35 → 0xca
rep("b15", "\x00\x41" ^ "\x00\x00"); // 结果可含 \0
rep("b16", "p" & "\x11");
rep("b17", "hello" | "\x00\x00\x00\x00\x00");
rep("b18", gettype("p" ^ "\x11"));   // "string"
rep("b19", gettype("5" ^ 2));        // "integer"
rep("b20", "abcd" ^ "ab");           // ^ MIN 实证（报告此处原写 max，zend 源码/实测均 MIN）
rep("b21", "abcd" ^ "\x00");
rep("b22", "a\x00c" & "abc");        // 二进制安全 &
