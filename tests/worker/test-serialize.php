<?php
// M5-b: serialize / unserialize —— 自有二进制格式 round-trip：
// 标量/数组/嵌套/backref 去重（DAG）/循环引用报错/对象 __cloneState 协议
// /默认 props 直填/__restoreCloneState 优先/enum 单例/closure 拒绝
use function Tphp\spawn;

// 1) 标量 round-trip
var_dump(unserialize(serialize(null)));
var_dump(unserialize(serialize(true)));
var_dump(unserialize(serialize(42)));
var_dump(unserialize(serialize(-17)));
var_dump(unserialize(serialize(3.5)));
var_dump(unserialize(serialize("hello 世界")));

// 2) 数组 round-trip（嵌套 + 混合键 + 插入序；=== 递归全等）
$a = [1, 'k' => 'v', 'nest' => [2, 3, 'x' => [4]]];
$r = unserialize(serialize($a));
var_dump($r === $a);
var_dump(count($r), $r['k'], $r['nest']['x'][0], $r[0]);

// 3) backref 去重：同一数组两处引用 → 恢复后 === 同实例
$shared = ['s'];
$dag = ['a' => $shared, 'b' => $shared];
$r2 = unserialize(serialize($dag));
var_dump($r2['a'] === $r2['b']);

// 4) 对象：__cloneState 导出 + 默认 props 直填恢复
class Point {
    public $x;
    public $y;
    function __construct($x, $y) { $this->x = $x; $this->y = $y; }
    function __cloneState(): array { return ['x' => $this->x, 'y' => $this->y]; }
}
$p = unserialize(serialize(new Point(3, 4)));
var_dump($p instanceof Point, $p->x, $p->y);

// 5) 循环引用（对象自引用）→ Error
class Node {
    public $next;
    function __cloneState(): array { return ['next' => $this->next]; }
}
$n = new Node();
$n->next = $n;
try {
    serialize($n);
    echo "no-error\n";
} catch (Error $e) {
    echo "circular\n";
}
echo "after-circular\n";

// 6) __restoreCloneState 优先（且构造器默认值为数组：defaults 深拷路径）
class Box {
    private $items;
    function __construct($items = []) { $this->items = $items; }
    function get() { return $this->items; }
    function __cloneState(): array { return ['items' => $this->items]; }
    function __restoreCloneState(array $s) {
        $this->items = array_merge($s['items'], ['restored']);
    }
}
$b = unserialize(serialize(new Box(['a', 'b'])));
var_dump($b->get());

// 7) enum case round-trip：恢复的是本线程单例（=== 同一对象）
enum Suit: string { case Hearts = 'H'; case Spades = 'S'; }
$e = unserialize(serialize(Suit::Hearts));
var_dump($e === Suit::Hearts, $e->value);

// 8) closure 可序列化（跨线程闭包支持）：函数索引 + 递归捕获
try {
    $s = serialize(function () { return 7; });
    $f2 = unserialize($s);
    var_dump($f2 instanceof Closure, $f2());
} catch (NotCloneableError $ex) {
    echo "not-cloneable\n";
}

// 9) 无 __cloneState 的对象 → NotCloneableError
class Plain { public $v = 1; }
try {
    serialize(new Plain());
    echo "no-error\n";
} catch (NotCloneableError $ex) {
    echo "plain-rejected\n";
}

// 10) 非法数据 → warning + false
var_dump(unserialize("\x00\x01garbage"));

echo "serialize-done\n";

// EXPECT: NULL
// EXPECT: bool(true)
// EXPECT: int(42)
// EXPECT: int(-17)
// EXPECT: float(3.5)
// EXPECT: string(12) "hello 世界"
// EXPECT: bool(true)
// EXPECT: int(3)
// EXPECT: string(1) "v"
// EXPECT: int(4)
// EXPECT: int(1)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: int(3)
// EXPECT: int(4)
// EXPECT: circular
// EXPECT: after-circular
// EXPECT: array(3) {
// EXPECT:   [0]=>
// EXPECT:   string(1) "a"
// EXPECT:   [1]=>
// EXPECT:   string(1) "b"
// EXPECT:   [2]=>
// EXPECT:   string(8) "restored"
// EXPECT: }
// EXPECT: bool(true)
// EXPECT: string(1) "H"
// EXPECT: bool(true)
// EXPECT: int(7)
// EXPECT: plain-rejected
// EXPECT: bool(false)
// EXPECT: serialize-done
