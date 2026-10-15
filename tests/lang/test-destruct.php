<?php
// __destruct 全路径（2026-10-15 P0-2 回归）
// 覆盖：rc 归零 / unset / 函数返回帧销毁 / 嵌套顺序 / 复活 / 构造失败不析构
// EXPECT: d1
// EXPECT: d2
// EXPECT: outer
// EXPECT: inner
// EXPECT: resurrect
// EXPECT: alive
// EXPECT: bool(true)
// EXPECT: ctor-caught
// EXPECT: end1
class B1 { public function __destruct() { echo "d1\n"; } }
class B2 { public function __destruct() { echo "d2\n"; } }
$a = new B1();
$a = null;                      // rc 归零立即析构
$b = new B2();
unset($b);                      // unset 立即析构

class End { public function __destruct() { echo "end1\n"; } }
$e = new End();                 // 脚本结束才析构

class Inner2 { public function __destruct() { echo "inner\n"; } }
class Outer2 {
    public $i;
    public function __construct() { $this->i = new Inner2(); }
    public function __destruct() { echo "outer\n"; }   // 外层先于内层
}
$o = new Outer2();
$o = null;

class Res { public function __destruct() { global $keep; $keep = $this; echo "resurrect\n"; } }
$keep = null;
function makeRes(): void { $r = new Res(); }           // 帧销毁触发析构
makeRes();
echo "alive\n";
var_dump($keep instanceof Res);                        // 复活后不再析构

class Bad {
    public function __construct() { throw new Exception("ctor"); }
    public function __destruct() { echo "never-dtor\n"; }   // 构造失败不析构
}
try { $x = new Bad(); } catch (Exception $ex) { echo "ctor-caught\n"; }
