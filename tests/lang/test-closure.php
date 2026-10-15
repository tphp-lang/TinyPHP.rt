<?php
// Closure 类 + 闭包 $this 捕获（2026-10-15 P1-3 回归）
// 历史：Closure 类不存在（fromCallable 报 Class not found）、instanceof
// Closure = false、闭包体内 $this 报 Undefined variable。
// EXPECT: 5
// EXPECT: 7
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: 20
// EXPECT: 20
// EXPECT: 20
// EXPECT: no
// EXPECT: fromCallable-ok
class C1 {
    private int $v = 5;
    public function maker(): Closure {
        return function() { return $this->v; };   // P1-3: $this 自动捕获
    }
}
$c1 = new C1();
$fn1 = $c1->maker();
echo $fn1(), "\n";

// 静态闭包不捕获 $this（对齐 PHP）——只验证可创建可调用
class C2 {
    private static int $sv = 6;
    public static function maker(): Closure { return function() { return 7; }; }
}
echo C2::maker()(), "\n";

$clo = function($x) { return $x * 2; };
var_dump($clo instanceof Closure);
var_dump(Closure::fromCallable("strlen") instanceof Closure);

// bindTo/bind 重绑定 $this + scope 决定 private 可见性（对齐 PHP）
class Src { private int $v = 10; public function maker(): Closure { return function() { return $this->v; }; } }
class D { private int $v = 20; public function demo(Closure $fn): void { echo $fn(), "\n"; } }
$fn = (new Src())->maker();
$d = new D();
$d->demo($fn->bindTo($d, D::class));
$d->demo(Closure::bind($fn, $d, D::class));
// call() 以 $newThis 自身类为作用域
echo $fn->call($d), "\n";
// 无 scope 时 private 不可达（原生 $fn 的作用域仍是 Src）
try { $fn->call($d); echo "no\n"; }
catch (Error $e) { echo "call-noscope-private-ok\n"; }

// fromCallable：字符串函数名 → 闭包
$len = Closure::fromCallable("strlen");
echo $len("hello") === 5 && $len("hi") === 2 ? "fromCallable-ok" : "bad", "\n";
