<?php
// EXPECT-EXIT: 255
// PHP 8.5 实测：`08`（legacy octal 数字位含 8/9）= 编译期
// "Invalid numeric literal" Parse error，零 stdout；tphp 对齐（lex_error）。
$x = 08;
echo "unreachable\n";
