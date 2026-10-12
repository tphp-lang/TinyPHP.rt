<?php
// 构造器：提升参数（含 readonly/protected/类型）、默认参数、构造器链
// EXPECT: name=Rex age=3
// EXPECT: tag=default
// EXPECT: dog: Rex says woof
// EXPECT: promoted-vis: hidden-vis

class Pet {
    public function __construct(public $name, public $age, $tag = "default") {
        $this->tag = $tag;
    }
    public $tag;
}
$a = new Pet("Rex", 3);
echo "name={$a->name} age={$a->age}", "\n";
echo "tag={$a->tag}", "\n";

class Animal {
    public function __construct(protected readonly string $name) {}
    public function getName() { return $this->name; }
    public function speak() { return "..."; }
}

class Dog extends Animal {
    public function __construct(string $name) {
        parent::__construct($name);
    }
    public function speak() { return "woof"; }
    public function intro() { return "dog: {$this->name} says " . $this->speak(); }
}

$d = new Dog("Rex");
echo $d->intro(), "\n";

class Hidden {
    public function __construct(private $secret) {}
    public function peek() { return $this->secret; }
}
$h = new Hidden("hidden-vis");
echo "promoted-vis: ", $h->peek(), "\n";
