<?php
// M5-c: Worker 克隆协议 —— 捕获值深拷 / 对象消息 __cloneState 跨线程
// round-trip / 子线程返回对象 / enum 消息
use Tphp\Worker;

// 1) 闭包捕获值跨线程深拷（标量 + 数组）
$prefix = "cap:";
$arr = [1, 2];
$w = new Worker(function () use ($prefix, $arr) {
    $me = Worker::self();
    while (($m = $me->recv()) !== false) {
        $me->post($prefix . $m . ":" . count($arr));
    }
});
$w->post("x");
var_dump($w->recv());
$w->stop();

// 2) 对象消息：父 → 子（子线程读其属性）
class Task {
    public $id;
    function __construct($id) { $this->id = $id; }
    function __cloneState(): array { return ['id' => $this->id]; }
}
$w2 = new Worker(function () {
    $me = Worker::self();
    while (($t = $me->recv()) !== false) {
        $me->post($t->id * 10);
    }
});
$w2->post(new Task(7));
var_dump($w2->recv());
$w2->stop();

// 3) 对象消息：子 → 父（父端恢复对象）
$w3 = new Worker(function () {
    $me = Worker::self();
    while (($m = $me->recv()) !== false) {
        $me->post(new Task(99));
    }
});
$w3->post("go");
$back = $w3->recv();
var_dump($back instanceof Task, $back->id);
$w3->stop();

// 4) enum 消息跨线程（单例序列化）
enum Level: int { case Low = 1; case High = 5; }
$w4 = new Worker(function () {
    $me = Worker::self();
    while (($l = $me->recv()) !== false) {
        $me->post($l->value * 2);
    }
});
$w4->post(Level::High);
var_dump($w4->recv());
$w4->stop();

echo "clone-done\n";

// EXPECT: string(7) "cap:x:2"
// EXPECT: int(70)
// EXPECT: bool(true)
// EXPECT: int(99)
// EXPECT: int(10)
// EXPECT: clone-done
