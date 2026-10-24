<?php
// EXPECT: instanceof=yes
// EXPECT: foreach=0:10,1:20,2:30,
// EXPECT: keyed=0:a,7:b,
// EXPECT: autolen=0:x,1:y,5:z,6:w,
// EXPECT: closure=ok
// EXPECT: early=first
// EXPECT: ret=42
// EXPECT: ok
// G1+G2: generators — foreach / auto+explicit keys / largest rule / closures /
// early return / getReturn

function gen3() {
    yield 10;
    yield 20;
    yield 30;
}
function keyed() {
    yield 0 => "a";
    yield 7 => "b";
}
function autolen() {
    yield "x";
    yield "y";
    yield 5 => "z";
    yield "w";   // auto key 从 largest 继续 → 6（PHP 规则）
}
function early() {
    yield "first";
    return;
    yield "never";
}
function withret() {
    yield 1;
    return 42;
}

$g = gen3();
echo "instanceof=", ($g instanceof Generator) ? "yes" : "no", "\n";

$s = "";
foreach (gen3() as $k => $v) $s .= "$k:$v,";
echo "foreach=$s\n";

$s = "";
foreach (keyed() as $k => $v) $s .= "$k:$v,";
echo "keyed=$s\n";

$s = "";
foreach (autolen() as $k => $v) $s .= "$k:$v,";
echo "autolen=$s\n";

$cgen = function () { yield "ok"; };
foreach ($cgen() as $v) echo "closure=$v\n";

foreach (early() as $v) echo "early=$v\n";

$wr = withret();
$wr->next();
echo "ret=", $wr->getReturn(), "\n";
echo "ok\n";
