<?php
// 刀7B（explode 元素 exact-alloc / cap=0）语义对拍矩阵。
// 四模逐字一致：默认（EXACT_ALLOC 开）/ TPHP_EXACT_ALLOC=0 / TPHP_JIT=0 / PHP 8.5。
// 覆盖：单字节快路、多字节通用路、尾分隔符/空段、limit 正/负、
// cap=0 元素的 .= 慢路、哈希/intern 键交互、str_split 保持原路径、值比较。

$s = "a,b,,c,";
$r = explode(",", $s);
echo "parts=", count($r), " join=", implode("|", $r), "\n";

$x = explode(",", "one,two,three", 2);
echo "limit2=", implode("/", $x), "\n";
// 注：负 limit 是 tphp 与 PHP 的既有语义差异（PHP 丢尾部元素、tphp 不丢），
// 与本刀无关、另案记账，故不纳入四模对拍（避免把存量差异写成假通过）。
$y = explode(",", "a,b,c", 3);
echo "limit3=", implode("/", $y), "\n";

$mb = explode("::", "a::b::c");
echo "multi=", implode("-", $mb), "\n";
$nl = explode("\n", "l1\nl2\nl3");
echo "nl=", implode(";", $nl), "\n";

// cap=0 元素被 .=：必须落慢路得到正确新串（不能原地写越界）
$e = explode(",", "aa,bb");
$e[0] .= "XX";
$e[1] .= "YY";
echo "concat=", $e[0], "/", $e[1], " len=", strlen($e[0]) + strlen($e[1]), "\n";

// 元素作为 hash 键（TStr.hash 计算/intern 指针路径）
$keys = ["aa" => 1, "bb" => 2];
$n = 0;
foreach (explode(",", "aa,bb,aa") as $k) $n += $keys[$k];
echo "keys=", $n, "\n";

$z = explode(",", "");
echo "empty=", count($z), " first='", $z[0], "'\n";

$m = explode("||", "p||q");
$m[1] .= "!";
echo "mbconcat=", $m[0], $m[1], "\n";

$sp = str_split("abcd", 2);
echo "split=", implode(":", $sp), "\n";

$h = explode(",", "v");
echo "cmp=", ($h[0] === "v" ? 1 : 0), "\n";
