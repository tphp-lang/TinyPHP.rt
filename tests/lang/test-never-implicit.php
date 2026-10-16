<?php
// B1: never 函数隐式返回 = 运行期可捕获 TypeError（对齐 PHP 逐字消息）
// EXPECT: in n
// EXPECT: TypeError: n(): never-returning function must not implicitly return
function n(): never { echo "in n\n"; }
try { n(); } catch (Throwable $e) { echo get_class($e), ": ", $e->getMessage(), "\n"; }
