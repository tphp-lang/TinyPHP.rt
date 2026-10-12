<?php
// 继承：parent:: 方法/属性、重写、多层链
// EXPECT: child-ctor
// EXPECT: base-ctor
// EXPECT: child.speak / base.hello
// EXPECT: child-ctor
// EXPECT: base-ctor
// EXPECT: grand: G-speak via C-speak
// EXPECT: area: 78.5
// EXPECT: circle: 78.5

class Base {
    public $tag = "base";
    public function __construct() { echo "base-ctor", "\n"; }
    public function hello() { return "base.hello"; }
    public function speak() { return "B-speak"; }
    public function combo() { return "child.speak / " . $this->hello(); }
}

class Child extends Base {
    public function __construct() {
        echo "child-ctor", "\n";
        parent::__construct();
    }
    public function speak() { return "C-speak"; }
    public function parentSpeak() { return parent::speak(); }
}

class Grand extends Child {
    public function speak() { return "G-speak via " . parent::speak(); }
}

$c = new Child();
echo $c->combo(), "\n";
$g = new Grand();
echo "grand: ", $g->speak(), "\n";

class Shape {
    protected $name = "shape";
    public function area() { return 0.0; }
    public function describe() { return "{$this->name}: {$this->area()}"; }
}

class Circle extends Shape {
    protected $name = "circle";
    public function __construct(public $r) {}
    public function area() { return 3.14 * $this->r * $this->r; }
}

$ci = new Circle(5);
echo "area: ", $ci->area(), "\n";
echo $ci->describe(), "\n";
