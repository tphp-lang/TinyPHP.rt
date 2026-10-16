<?php
// B7: return 穿越多层嵌套 finally——外层 finally 逐层执行后才返回（PHP 对拍 q7）
// EXPECT: a: f1
// EXPECT: f2
// EXPECT: inner
// EXPECT: b: d1
// EXPECT: d2
// EXPECT: d3
// EXPECT: deep
// EXPECT: c: c: x
// EXPECT: cf1
// EXPECT: cf2
// EXPECT: from-catch
// EXPECT: d: p1
// EXPECT: p2
// EXPECT: pos / p2
// EXPECT: neg
// EXPECT: e: ef
// EXPECT: e
function b7a() {
    try {
        try { return "inner"; } finally { echo "f1\n"; }
    } finally { echo "f2\n"; }
}
echo "a: ", b7a(), "\n";

function b7b() {
    try {
        try {
            try { return "deep"; } finally { echo "d1\n"; }
        } finally { echo "d2\n"; }
    } finally { echo "d3\n"; }
}
echo "b: ", b7b(), "\n";

function b7c() {
    try {
        try { throw new RuntimeException("x"); } catch (Throwable $e) { echo "c: ", $e->getMessage(), "\n"; return "from-catch"; } finally { echo "cf1\n"; }
    } finally { echo "cf2\n"; }
}
echo "c: ", b7c(), "\n";

function b7d($n) {
    try {
        if ($n > 0) { try { return "pos"; } finally { echo "p1\n"; } }
        return "neg";
    } finally { echo "p2\n"; }
}
echo "d: ", b7d(1), " / ";
echo b7d(-1), "\n";

function b7e() {
    try { return "e"; } finally { echo "ef\n"; }
}
echo "e: ", b7e(), "\n";
