<?php
$name = "world";
// EXPECT: hello world
echo "hello $name", "\n";
$n = 5;
// EXPECT: n=5
echo "n=$n", "\n";
$a = ["k" => "v"];
// EXPECT: vv
echo "{$a["k"]}{$a['k']}", "\n";
$b = [10];
// EXPECT: 10
echo "$b[0]", "\n";
// EXPECT: a	b
echo "a\tb", "\n";
// EXPECT: "quoted"
echo "\"quoted\"", "\n";
// EXPECT: $notvar
echo "\$notvar", "\n";
// EXPECT: C:\path
echo "C:\\path", "\n";
// EXPECT: A
echo "\x41", "\n";
// EXPECT: B
echo "\u{42}", "\n";
// EXPECT: $name
echo '$name', "\n";
// EXPECT: xy
echo "x" . "y", "\n";
$s = "a";
$s .= "b";
// EXPECT: ab
echo $s, "\n";
$c = 4;
// EXPECT: 42
echo "{$c}0" + 2, "\n";
// EXPECT: a-b
echo "a" . "-" . "b", "\n";
// interpolation with call
function who() { return "me"; }
// EXPECT: who=me
echo "who=", who(), "\n";
// EXPECT: x:10
echo "x:$b[0]", "\n";
// newline escape inside interpolation part
$t = "T";
// EXPECT: [T]
echo "[$t]", "\n";
