<?php
// trait：方法绑定、as 改名/可见性、insteadof 冲突消解
// EXPECT: a-hello: Hello from A
// EXPECT: b-hello: Hello from B
// EXPECT: renamed: Renamed hello
// EXPECT: c-hello: T1.hello
// EXPECT: c-aliased: T2.hello
// EXPECT: prop: shared

trait Hello1 {
    public function hello() { return "Hello from A"; }
}

trait Hello2 {
    public function hello() { return "Hello from B"; }
}

trait Renamer {
    public function hello() { return "Renamed hello"; }
}

class A {
    use Hello1;
}
class B {
    use Hello2;
}
class Renamed {
    use Renamer {
        hello as hi;
    }
}

$a = new A();
echo "a-hello: ", $a->hello(), "\n";
$b = new B();
echo "b-hello: ", $b->hello(), "\n";
$rn = new Renamed();
echo "renamed: ", $rn->hi(), "\n";

trait T1 {
    public function hello() { return "T1.hello"; }
    public function alt() { return "T1.alt"; }
}
trait T2 {
    public function hello() { return "T2.hello"; }
    public function alt() { return "T2.alt"; }
}

class Both {
    use T1, T2 {
        T1::hello insteadof T2;
        T2::alt insteadof T1;
        T2::hello as aliased;
    }
}

$c = new Both();
echo "c-hello: ", $c->hello(), "\n";
echo "c-aliased: ", $c->aliased(), "\n";

trait Counter {
    public $n = 0;
    public function bump() { return ++$this->n; }
}

class UsingProp {
    use Counter;
}
$u = new UsingProp();
$u->bump();
$u->bump();
echo "prop: ", $u->bump() == 3 ? "shared" : "broken", "\n";
