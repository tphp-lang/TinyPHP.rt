<?php
// EXPECT: a=511
// EXPECT: b=493
// EXPECT: c=420
// EXPECT: d=8
// EXPECT: e=511
// EXPECT: f=0
// EXPECT: g=16
// EXPECT: h=5
// EXPECT: i=63
// EXPECT: j=512
// EXPECT: k=0.5x=0.5
// EXPECT: END
// legacy 八进制字面量（11-11 CI pkg-scripts 根因：曾全走十进制 0777=777、
// mkdir 0777 落 0o411 无 w 位 EACCES）——与 PHP 8.5 逐值对拍。
echo "a=", 0777, "\n";            // 511
echo "b=", 0755, "\n";            // 493
echo "c=", 0644, "\n";            // 420
echo "d=", 010, "\n";             // 8
echo "e=", 0o777, "\n";           // PHP 8.1 显式八进制（既有支持）
echo "f=", 0, "\n";               // 纯 0
echo "g=", 0x10, "\n";            // 十六进制不受影响
echo "h=", 0b101, "\n";           // 二进制不受影响
echo "i=", 0_77, "\n";            // 带分隔 legacy octal：0_77 = 077 = 63（PHP 同值）
echo "j=", 0777 + 1, "\n";        // 表达式上下文 = 512
$k = 0.5;
echo "k=0.5x=", $k, "\n";         // 0.5 不受 octal 判定影响
echo "END\n";
