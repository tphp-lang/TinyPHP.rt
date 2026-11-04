<?php
// take 门消融计时（11-04 发射器冗余刀1）：镜像 bench array-read
// 跑法：tphp run tools\ablate_take.php；对照 TPHP_ABL_TAKE=1（门旁路）
$c = function () {
    $a = [];
    for ($i = 0; $i < 1000; $i++) { $a[] = $i; }
    $x = 0;
    for ($r = 0; $r < 100; $r++) {
        for ($i = 0; $i < 1000; $i++) { $x = $a[$i]; }
    }
    return $x;
};
$t0 = microtime(true);
for ($k = 0; $k < 1000; $k++) { $c(); }
$t1 = microtime(true);
echo ($t1 - $t0), "\n";
