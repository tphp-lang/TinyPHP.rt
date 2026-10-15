<?php
// 引用环回收 + 环中 __destruct（2026-10-15 P0-2 回归）
// 对象环经 gc_collect_cycles 回收，两个析构器各跑一次；
// 用全局日志 + sort 锁定集合而非顺序（环内析构次序不锁定）。
// EXPECT: before
// EXPECT: freed=2
// EXPECT: c1,c2
// EXPECT: after
$log = [];
class N {
    public $p;
    public $id;
    public function __construct($id) { $this->id = $id; }
    public function __destruct() { global $log; $log[] = "c{$this->id}"; }
}
function cyc(): void {
    $a = new N(1);
    $b = new N(2);
    $a->p = $b;
    $b->p = $a;
}
cyc();
echo "before\n";
echo "freed=", gc_collect_cycles(), "\n";
sort($log);
echo implode(",", $log), "\n";
echo "after\n";
