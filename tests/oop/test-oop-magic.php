<?php
// 魔术方法：__get/__set/__isset/__unset/__call/__callStatic
//            __toString/__invoke/__clone
// EXPECT: set: color=red
// EXPECT: get: RED
// EXPECT: isset-color: yes
// EXPECT: isset-missing: no
// EXPECT: unset-color: gone
// EXPECT: call: shade(red, dark)
// EXPECT: callStatic: SHADE(blue)
// EXPECT: tostring: STR!
// EXPECT: concat: id-STR!
// EXPECT: invoke: inv:7
// EXPECT: clone-hook: cloned
// EXPECT: clone-copy: 5,9
// EXPECT: clone-independent: 9,5

class Tint {
    private $data = [];

    public function __set($name, $value) {
        $this->data[$name] = $value;
    }

    public function __get($name) {
        $v = $this->data[$name] ?? null;
        return $v === null ? null : strtoupper($v);
    }

    public function __isset($name) {
        return isset($this->data[$name]);
    }

    public function __unset($name) {
        unset($this->data[$name]);
    }

    public function __call($name, $args) {
        return "$name({$args[0]}, {$args[1]})";
    }

    public function peekRaw($k) { return $this->data[$k]; }

    public static function __callStatic($name, $args) {
        return strtoupper($name) . "({$args[0]})";
    }
}

$t = new Tint();
$t->color = "red";
echo "set: color={$t->peekRaw("color")}", "\n";
echo "get: ", $t->color, "\n";
echo "isset-color: ", isset($t->color) ? "yes" : "no", "\n";
echo "isset-missing: ", isset($t->nope) ? "yes" : "no", "\n";
unset($t->color);
echo "unset-color: ", isset($t->color) ? "kept" : "gone", "\n";
echo "call: ", $t->shade("red", "dark"), "\n";
echo "callStatic: ", Tint::SHADE("blue"), "\n";

class Badge {
    public $id = 42;
    public function __toString(): string { return "STR!"; }
}
$b = new Badge();
echo "tostring: ", $b, "\n";
echo "concat: id-" . $b, "\n";

class Invokable {
    public function __invoke($x) { return "inv:$x"; }
}
$i = new Invokable();
echo "invoke: ", $i(7), "\n";

class Cell {
    public $v = 1;
    public function __clone() { echo "clone-hook: cloned", "\n"; }
}
$c = new Cell();
$c->v = 5;
$c2 = clone $c;
$c2->v = 9;
echo "clone-copy: {$c->v},{$c2->v}", "\n";
echo "clone-independent: {$c2->v},{$c->v}", "\n";
