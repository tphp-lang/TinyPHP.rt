<?php
// OOP 基础：类/属性/方法/$this/默认值/属性插值
// EXPECT: point: 3,4
// EXPECT: moved: 10,4
// EXPECT: label=Box count=2
// EXPECT: sum=60
// EXPECT: Counter=7

class Point {
    public $x;
    public $y;
    public $label = "pt";
    public function __construct($x, $y) {
        $this->x = $x;
        $this->y = $y;
    }
    public function move($dx, $dy) {
        $this->x += $dx;
        $this->y += $dy;
        return $this;
    }
    public function show() {
        return "point: {$this->x},{$this->y}";
    }
}

$p = new Point(3, 4);
echo $p->show(), "\n";
$p->move(7, 0);
echo "moved: {$p->x},{$p->y}", "\n";

class Box {
    public $items = [];
    public $count = 0;
    public $label = "Box";
    public function add($v) {
        $this->items[] = $v;
        $this->count++;
    }
}
$b = new Box();
$b->add("a");
$b->add("b");
echo "label={$b->label} count={$b->count}", "\n";

class Sum {
    public $total = 0;
    public function add($n) { $this->total += $n; }
}
$s = new Sum();
$s->add(10);
$s->add(20);
$s->add(30);
echo "sum={$s->total}", "\n";

class Counter {
    public $n = 7;
}
$c = new Counter();
echo "Counter={$c->n}", "\n";
