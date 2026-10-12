<?php
$g = 10;
function readg() { global $g; return $g; }
function writeg() { global $g; $g = 20; }
// EXPECT: 10
echo readg(), "\n";
writeg();
// EXPECT: 20
echo $g, "\n";
// EXPECT: 20
echo readg(), "\n";
$h = [1];
function pushh() { global $h; $h[] = 2; }
pushh();
// EXPECT: int(2)
var_dump(count($h));
function hasg() { global $g; return isset($g); }
// EXPECT: bool(true)
var_dump(hasg());
function clearg() { global $g; unset($g); }
clearg();
// PHP: unset inside a function only breaks the alias
// EXPECT: 20
echo $g, "\n";
// main-frame variable visible without any global decl
$visible = "yes";
function seev() { global $visible; return $visible; }
// EXPECT: yes
echo seev(), "\n";
// global read of undefined is a fatal; alias write creates it
function makel() { global $made; $made = 5; }
makel();
// EXPECT: int(5)
var_dump($made);
// two functions sharing a global
function inc() { global $counter; $counter = $counter ?? 0; $counter++; }
inc();
inc();
inc();
// EXPECT: int(3)
var_dump($counter);
