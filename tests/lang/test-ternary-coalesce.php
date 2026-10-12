<?php
$a = 5;
// EXPECT: int(5)
var_dump($a ?: "d");
$b = 0;
// EXPECT: int(2)
var_dump($b ?: 2);
// EXPECT: string(1) "t"
var_dump(true ? "t" : "f");
// EXPECT: string(1) "f"
var_dump(false ? "t" : "f");
// EXPECT: int(1)
var_dump(true ? 1 : 2);
// EXPECT: string(4) "mid1"
var_dump(false ? "a" : (true ? "mid1" : "mid2"));
// short ternary chain
$c = "";
// EXPECT: string(3) "alt"
var_dump($c ?: "alt");
// ?? chain
// EXPECT: string(1) "d"
var_dump($missing ?? "d");
$v = "v";
// EXPECT: string(1) "v"
var_dump($v ?? "d");
// EXPECT: string(1) "n"
var_dump(null ?? "n");
// EXPECT: string(1) "z"
var_dump($nope ?? $nada ?? "z");
$dd = ["k" => "kv"];
// EXPECT: string(2) "kv"
var_dump($dd["k"] ?? "def");
// EXPECT: string(3) "def"
var_dump($dd["no"] ?? "def");
$gone = 1;
unset($gone);
// EXPECT: string(2) "un"
var_dump($gone ?? "un");
// ??= via expansion
$opt ??= "assigned";
// EXPECT: string(8) "assigned"
var_dump($opt);
$opt2 = "kept";
$opt2 ??= "not-me";
// EXPECT: string(4) "kept"
var_dump($opt2);
// nested ternary both branches (parenthesized per PHP 8 rule)
// EXPECT: int(3)
var_dump(true ? (false ? 1 : 3) : 4);
