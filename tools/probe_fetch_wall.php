<?php
// FETCH 结构墙勘察（11-09 下一刀）：与 bench array-read 同形态热循环，
// TPHP_JIT_DEBUG=2 dump IR 看 spec=1 下 FETCH→ADD 之间残余结构。
$f = function ($a, $n) {
    $s = 0;
    $i = 0;
    while ($i < $n) {
        $s += $a[$i];
        $i++;
    }
    return $s;
};
$a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
$acc = 0;
for ($t = 0; $t < 200; $t++) $acc = $f($a, 10);
echo $acc, "\n";
