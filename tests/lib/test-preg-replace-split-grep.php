<?php
// EXPECT: replace=T-m-y-
// EXPECT: backref=world hello!
// EXPECT: limit=XXa-b
// EXPECT: nomatch=x1x2x
// EXPECT: split=a,b,c
// EXPECT: split-noempty=a,b
// EXPECT: split-delim=a,-,b
// EXPECT: grep=0:cat
// EXPECT: grep-invert=1:dog
// EXPECT: ok
// P3: preg_replace / preg_split / preg_grep

echo "replace=", preg_replace("/o/", "-", "Tomoyo"), "\n";

echo "backref=", preg_replace("/(\w+) (\w+)/", "$2 $1!", "hello world"), "\n";

echo "limit=", preg_replace("/a/", "X", "aaa-b", 2), "\n";

echo "nomatch=", preg_replace("/z/", "Y", "x1x2x"), "\n";

$p = preg_split("/,/", "a,b,c");
echo "split=", $p[0], ",", $p[1], ",", $p[2], "\n";

$p = preg_split("/,/", "a,,b",  -1, 1);   /* NO_EMPTY=1 */
echo "split-noempty=", $p[0], ",", $p[1], "\n";

$p = preg_split("/(-)/", "a-b", -1, 2);   /* DELIM_CAPTURE=2 */
echo "split-delim=", $p[0], ",", $p[1], ",", $p[2], "\n";

$g = preg_grep("/cat/", ["cat", "dog", "catalog"]);
foreach ($g as $k => $v) { echo "grep=$k:$v\n"; break; }

$gi = preg_grep("/cat/", ["cat", "dog"], 1);  /* INVERT */
foreach ($gi as $k => $v) { echo "grep-invert=$k:$v\n"; break; }
echo "ok\n";
