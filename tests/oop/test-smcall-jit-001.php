<?php
// 批3-3（INIT_SMCALL 静态方法直调）：可判子集进 JIT（self/parent/字面量
// Cls::），static::/动态名/enum builtin/native/__callStatic/跨类 private
// 拒编回退解释器（完整语义）。raise 场景在 JIT 外 catch。
class Sm {
    public static function add($a, $b) { return $a + $b; }
    private static function secret() { return 7; }
    protected static function prot() { return 11; }
    public static function useSelf() { return self::add(2, 3) + self::prot(); }
    public static function __callStatic($n, $a) { return "magic:$n"; }
    public static function lsb() { return static::class; }
}
class SmChild extends Sm {
    public static function viaParent() { return parent::prot() + 100; }
}
enum SmE: string { case A = "a"; case B = "b"; }

// EXPECT: 20
function drive_add($n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) $s = Sm::add($s, 2);
    return $s;
}
echo drive_add(10), "\n";
// EXPECT: 16
function drive_self($n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) $s += Sm::useSelf();
    return $s;
}
echo drive_self(1), "\n";
// EXPECT: 111
function drive_parent() { return SmChild::viaParent(); }
echo drive_parent(), "\n";
// EXPECT: 1.5
function drive_float() { return Sm::add(0.5, 1); }   // 非 INT 实参 → deopt → 解释器
echo drive_float(), "\n";
// EXPECT: 33
function drive_child_add($n) {
    $s = 0;
    for ($i = 0; $i < $n; $i++) $s = SmChild::add($s, 3);   // 继承的静态方法
    return $s;
}
echo drive_child_add(11), "\n";
// JIT'd 实例方法内调他类静态（方法帧 poff=1 布局的直调发起方）
class SmDriver {
    public $k;
    function __construct($k) { $this->k = $k; }
    function run($n) {
        $s = 0;
        for ($i = 0; $i < $n; $i++) $s = Sm::add($s, $this->k);
        return $s;
    }
}
// EXPECT: 50
$d = new SmDriver(5);
echo $d->run(10), "\n";
// 跨类 private：类无 __callStatic 时拒编 → 解释器可见性 raise
// （有 __callStatic 时 PHP 语义=接管"不可访问"而不仅是未定义——见下方用例）
class SmVis {
    private static function secret() { return 7; }
    protected static function prot() { return 11; }
}
// EXPECT: Error: Call to private method SmVis::secret() from global scope
function drive_secret() { return SmVis::secret(); }
try { drive_secret(); } catch (Throwable $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
class SmVisOther {
    public static function go() { return SmVis::prot(); }
}
// EXPECT: Error: Call to protected method SmVis::prot() from scope SmVisOther
function drive_prot_stranger() { return SmVisOther::go(); }
try { drive_prot_stranger(); } catch (Throwable $e) {
    echo get_class($e), ": ", $e->getMessage(), "\n";
}
// __callStatic 接管"不可访问"（PHP 语义对拍：private 调用进 magic 不 raise）
// EXPECT: magic:secret
function drive_inaccessible() { return Sm::secret(); }
echo drive_inaccessible(), "\n";
// __callStatic：未定义静态方法 → 拒编 → 解释器兜底
// EXPECT: magic:nope
function drive_magic() { return Sm::nope(1); }
echo drive_magic(), "\n";
// static:: LSB：拒编 → 解释器
// EXPECT: SmChild
function drive_lsb() { return SmChild::lsb(); }
echo drive_lsb(), "\n";
// enum ::from builtin：拒编 → 解释器
// EXPECT: a
function drive_from($v) { return SmE::from($v)->value; }
echo drive_from("a"), "\n";
// EXPECT: all-ok
echo "all-ok\n";
