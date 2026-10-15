<?php
// P3 校验与语义落地（2026-10-15 回归）：ctor 可见性 / 接口方法体拒绝 /
// readonly class / override 签名 / 自工厂 / 惰性类常量 / 子类静态属性独立槽
// EXPECT: priv-ctor-ok
// EXPECT: prot-ok
// EXPECT: ro-cls-ok
// EXPECT: 1
// EXPECT: self-factory-ok
// EXPECT: add-optional-ok
// EXPECT: ret-ok
// EXPECT: 1
// EXPECT: 1 2
// EXPECT: 1 9
// EXPECT: 5 9
// EXPECT: 5
// EXPECT: xy xy!
class Singleton { private function __construct() {} }
try { new Singleton(); echo "no\n"; } catch (Error $e) { echo "priv-ctor-ok\n"; }

class BaseP { protected function __construct() {} }
class SubP extends BaseP { static function make(): BaseP { return new SubP(); } }
echo SubP::make() instanceof BaseP ? "prot-ok\n" : "bad\n";

readonly class R { public int $v = 1; }
$r = new R();
try { $r->v = 2; echo "no\n"; } catch (Error $e) { echo "ro-cls-ok\n"; }
echo $r->v, "\n";

// 自工厂（本段修复：static 方法体内 new SameClass 编译期可查）
class Solo {
    public int $n = 1;
    private function __construct() {}
    static function make(): Solo { return new Solo(); }
}
echo Solo::make()->n === 1 ? "self-factory-ok\n" : "bad\n";

// override 签名：子类增可选参数 OK；参数类型收窄为编译期 fatal
//（tphp_fatal_src 无法在同文件 try/catch——负例由 q1/q5 探针验证，
//  注意：try 块内的类声明会被 pass_classes 漏掉（既有缺口，见 TODO））
class OA { public function f($a) {} }
class OB extends OA { public function f($a, $b = 1) {} }
echo "add-optional-ok\n";
class OC extends OA { public function g(): int { return 1; } }
echo (new OC())->g() === 1 ? "ret-ok\n" : "bad\n";

// 惰性类常量表达式：前向引用 / 链式 self::
class CC { const A = self::B; const B = 1; }
echo CC::A, "\n";

// 子类静态属性重声明 = 独立存储槽
class SA { public static int $x = 1; public static function getSelf(): int { return self::$x; } }
class SB extends SA { public static int $x = 2; }
echo SA::$x, ' ', SB::$x, "\n";
SB::$x = 9;
echo SA::$x, ' ', SB::$x, "\n";
SA::$x = 5;
echo SA::$x, ' ', SB::$x, "\n";
echo SB::getSelf(), "\n";

// 类常量链式引用
class CD { const A = "x" . self::B; const B = "y"; const D = self::A . "!"; }
echo CD::A, ' ', CD::D, "\n";
