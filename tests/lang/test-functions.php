<?php
// EXPECT: 6
// EXPECT: 12
// EXPECT: 3
// EXPECT: 4
// EXPECT: 6
// EXPECT: 7
// EXPECT: 7
// EXPECT: 5
// EXPECT: 5
// EXPECT: ok-goto
function def1($a, $b = 2) { return $a * $b; }
echo def1(3), "\n", def1(3, 4), "\n";
function va(...$a) { return count($a); }
echo va(1, 2, 3), "\n";
function np($a, $b) { return $a - $b; }
echo np(b: 1, a: 5), "\n";
function sp($a, $b, $c) { return $a + $b + $c; }
$args = [1, 2, 3];
echo sp(...$args), "\n";
$add = fn($a, $b) => $a + $b;
echo $add(3, 4), "\n";
$sfn = static function () { return 7; };
echo $sfn(), "\n";
$fn = 'np';
echo $fn(9, 4), "\n";
$fcc = strlen(...);
echo $fcc("hello"), "\n";
echo "ok-goto", "\n";
goto done;
echo "skipped", "\n";
done:
