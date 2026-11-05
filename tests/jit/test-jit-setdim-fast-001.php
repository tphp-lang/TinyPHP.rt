<?php
// 刀4 keyed 覆盖写快路（TPHP_SETDIM_FAST）边界：fast 条件任一不满足必须
// 落慢路完整兜底，输出与解释器/原 helper 逐字一致。
// EXPECT: --- setdim-fast ---
// EXPECT: 10,99,30
// EXPECT: str
// EXPECT: str,100,30
// EXPECT: 1,2,50 n=3
// EXPECT: 66 n=3
// EXPECT: 23
// EXPECT: 55
// EXPECT: 42
// EXPECT: 91
// EXPECT: 0=1 2=3 1=20
// EXPECT: new
// EXPECT: 5
// EXPECT: 7
// EXPECT: 126 64
// EXPECT: 8
// EXPECT: dtors=299
// EXPECT: END

echo "--- setdim-fast ---\n";

// 1) packed INT 覆盖（fast 主形态）
$a = [10, 20, 30];
$a[1] = 99;
echo implode(",", $a), "\n";

// 2) 串值覆盖（val.tag<T_STR 不满足 → 慢路）
$a[0] = "str";
echo $a[0], "\n";

// 3) RMW 形态（fast）
$a[1] = $a[1] + 1;
echo implode(",", $a), "\n";

// 4) 越界 int 键（≥n → 慢路，grow/hash 语义）
$b = [1, 2];
$b[5] = 50;
echo $b[0], ",", $b[1], ",", $b[5], " n=", count($b), "\n";

// 5) 负键（ULT 假 → 慢路 hash）
$c = [7, 8];
$c[-1] = 66;
echo $c[-1], " n=", count($c), "\n";

// 6) 字符串键 assoc 覆盖/新增（慢路）
$d = ["x" => 1];
$d["x"] = 2;
$d["y"] = 3;
echo $d["x"], $d["y"], "\n";

// 7) 数字串键：慢路 norm → int → packed 覆盖
$e = [1];
$e["1"] = 55;
echo $e[1], "\n";

// 8) autoviv：未定义变量（UNDEF）base → 慢路建数组
// （注：tphp 对 `$x=null` 写维报 TypeError 而 PHP autoviv——存量 parity
//   差，三模一致非本刀引入，不入本测试）
$f[0] = 42;
echo $f[0], "\n";

// 9) COW：rc>1 → 慢路 copy，原数组不动
$g = [1, 2];
$h = $g;
$g[0] = 9;
echo $g[0], $h[0], "\n";

// 10) unset 打破 packed 后回写（慢路，洞/重排语义；打印迭代序键值）
$i = [1, 2, 3];
unset($i[1]);
$i[1] = 20;
$parts = [];
foreach ($i as $kk => $vv) $parts[] = "$kk=$vv";
echo implode(" ", $parts), "\n";

// 11) 旧值为串（覆盖前必须释放，门路径）
$s = ["k" => "old"];
$s["k"] = "new";
echo $s["k"], "\n";

// 12) 旧值为对象（释放走 dtor，不得崩）
$o = ["p" => new class {
    public $v = 1;
    function __destruct() { }
}];
$o["p"] = 5;
echo $o["p"], "\n";

// 13) bool 键 norm → int 1
$k = [true => 1];
$k[1] = 7;
echo $k[1], "\n";

// 14) 大数组顺序全覆盖（dcap 内，快线批量路径）
$m = [];
for ($x = 0; $x < 64; $x++) $m[] = $x;
for ($x = 0; $x < 64; $x++) $m[$x] = $x * 2;
echo $m[63], " ", count($m), "\n";

// 15) CONST 键覆盖
$n = [3 => 1];
$n[3] = 8;
echo $n[3], "\n";

// 16) 快路释放旧引用值：对象旧值 + 标量新值（fast 条件满足）——
// 每次覆盖必须触发旧对象 dtor（漏释放 = 299→298 计数不足；双释放 = 崩）。
// 注：echo 时 299 而非 PHP 的 300——tphp 主帧 TMP 槽持有最后一次 new 的
// 引用直到槽复用/帧结束（PHP 语句级 FREE_TMP），存量 temp 生命周期差，
// 默认开/SETDIM=0/解释器三模恒等非本刀引入。
class D4 { static $n = 0; function __destruct() { D4::$n++; } }
$p = [];
for ($x = 0; $x < 300; $x++) {
    $p[0] = new D4();
    $p[0] = $x;
}
gc_collect_cycles();   /* 冲刷 tphp dtor 延迟队列（mem.c g_dtor_defer），对齐 PHP 的 300 */
echo "dtors=", D4::$n, "\n";

echo "END\n";
