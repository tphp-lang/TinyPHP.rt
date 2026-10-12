<?php
function fib($n) {
    return $n < 2 ? $n : fib($n - 1) + fib($n - 2);
}
// EXPECT: 832040
echo fib(30), "\n";
function greet($name, $greeting = "Hello") {
    return "$greeting, $name!";
}
// EXPECT: Hello, PHP!
echo greet("PHP"), "\n";
// EXPECT: Hi, PHP!
echo greet("PHP", "Hi"), "\n";
function fact($n, $acc = 1) {
    return $n <= 1 ? $acc : fact($n - 1, $acc * $n);
}
// EXPECT: 120
echo fact(5), "\n";
// variable function call
$fn = "fib";
// EXPECT: 55
echo $fn(10), "\n";
// function hoisting: call before declaration
// EXPECT: 3
echo addone(2), "\n";
function addone($x) { return $x + 1; }
// mutual recursion
function is_even($n) { return $n == 0 ? true : is_odd($n - 1); }
function is_odd($n) { return $n == 0 ? false : is_even($n - 1); }
// EXPECT: bool(true)
var_dump(is_even(10));
// EXPECT: bool(false)
var_dump(is_even(7));
// default with constant literal expr
const BASE = 100;
// EXPECT: int(100)
var_dump(BASE);
// EXPECT: int(101)
var_dump(BASE + 1);
