<?php
// EXPECT: col=a,c
// EXPECT: col_idx_a=a
// EXPECT: col_idx_c=c
// EXPECT: comb=x:10,y:20
// EXPECT: chunk_n=3
// EXPECT: chunk0=1,2
// EXPECT: fill=5,5,5
// EXPECT: strtr3=Hellx Wxrld
// EXPECT: strtr_pairs=Hello World
// EXPECT: ucwords=Hello World Foo
// EXPECT: nl2br=a<br />
// EXPECT: b
// EXPECT: tags=bold text
// EXPECT: html=a &lt;b&gt;
// EXPECT: ok

$rows = [
    ["id" => 1, "name" => "a"],
    ["id" => 2, "name" => "b"],
    ["id" => 3, "name" => "c"],
];
$names = array_column($rows, "name");
echo "col=", $names[0], ",", $names[2], "\n";

$idx = array_column($rows, "name", "id");
echo "col_idx_a=", $idx[1], "\n";
echo "col_idx_c=", $idx[3], "\n";

$comb = array_combine(["x", "y"], [10, 20]);
echo "comb=x:", $comb["x"], ",y:", $comb["y"], "\n";

$chunks = array_chunk([1, 2, 3, 4, 5], 2);
echo "chunk_n=", count($chunks), "\n";
echo "chunk0=", $chunks[0][0], ",", $chunks[0][1], "\n";

$f = array_fill(0, 3, 5);
echo "fill=", $f[0], ",", $f[1], ",", $f[2], "\n";

echo "strtr3=", strtr("Hello World", "o", "x"), "\n";
echo "strtr_pairs=", strtr("Hello World", ["Hello" => "Hello", "World" => "World"]), "\n";

echo "ucwords=", ucwords("hello world foo"), "\n";
echo "nl2br=", nl2br("a\nb"), "\n";
echo "tags=", strip_tags("<b>bold</b> text"), "\n";
echo "html=", htmlspecialchars("a <b>"), "\n";
echo "ok\n";
