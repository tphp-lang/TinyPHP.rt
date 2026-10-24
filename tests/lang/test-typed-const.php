<?php
// EXPECT: plain=42
// EXPECT: typed=hello
// EXPECT: union-ok=99
// EXPECT: thunk-ok=3
// EXPECT: reject-ok
// EXPECT: ok
// C2: typed class constants（PHP 8.3）
class C {
    const PLAIN = 42;                  // 无类型（向后兼容）
    const string TYPED = "hello";
    const int|string UNION = 99;
    const OTHER = 1;
    const THUNK = self::OTHER + 2;     // 惰性 thunk 求值后仍校验
    const int BAD = "nope";            // 定义合法，取用时 TypeError
}
echo "plain=", C::PLAIN, "\n";
echo "typed=", C::TYPED, "\n";
echo "union-ok=", C::UNION, "\n";
echo "thunk-ok=", C::THUNK, "\n";
try {
    $x = C::BAD;
    echo "reject-FAIL\n";
} catch (TypeError $e) {
    echo "reject-ok\n";
}
echo "ok\n";
