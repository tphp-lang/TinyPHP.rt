<?php
// 类型声明运行期校验（2026-10-15 P1-1 回归）
// 覆盖：typed 属性（实例/静态/readonly/trait/union/可空）、参数类型、
// 返回类型、变参元素、静态属性直接赋值（self::/C::/static::）。
// 语义：全局 strict（PLAN/01 既定）+ int→float 宽化。
// EXPECT: 1 null 5 7
// EXPECT: 42
// EXPECT: str
// EXPECT: now-str
// EXPECT: 100
// EXPECT: caught-prop
// EXPECT: caught-param
// EXPECT: caught-ret
// EXPECT: 3
// EXPECT: caught-vararg
// EXPECT: 3 4
// EXPECT: 9
// EXPECT: trait-caught
// EXPECT: 3
// EXPECT: 2.5
class P {
    public int $n = 1;
    public ?string $s = null;
    public int|string $u = 5;
    public static int $cnt = 0;
    public readonly int $ro;
    public function __construct(int $ro) { $this->ro = $ro; }
}
$p = new P(7);
echo $p->n, ' ', $p->s ?? 'null', ' ', $p->u, ' ', $p->ro, "\n";
$p->n = 42; echo $p->n, "\n";
$p->s = "str"; echo $p->s, "\n";
$p->u = "now-str"; echo $p->u, "\n";
P::$cnt = 100; echo P::$cnt, "\n";

try { $p->n = "bad"; echo "no\n"; } catch (TypeError $e) { echo "caught-prop\n"; }

function needInt(int $x): int { return $x; }
try { needInt("bad"); echo "no\n"; } catch (TypeError $e) { echo "caught-param\n"; }
function badRet(): string { return 42; }
try { badRet(); echo "no\n"; } catch (TypeError $e) { echo "caught-ret\n"; }
function sum(int ...$nums): int { $s = 0; foreach ($nums as $n) $s += $n; return $s; }
echo sum(1, 2), "\n";
try { sum(1, "x"); echo "no\n"; } catch (TypeError $e) { echo "caught-vararg\n"; }

trait T { public int $tv = 3; public static int $ts = 4; }
class U { use T; }
$u = new U();
echo $u->tv, ' ', U::$ts, "\n";
U::$ts = 9; echo U::$ts, "\n";
try { $u->tv = "x"; echo "no\n"; } catch (TypeError $e) { echo "trait-caught\n"; }

class Widener { public function f(float $x): float { return $x; } }
$w = new Widener();
echo $w->f(3), "\n";   // int → float 宽化允许（PLAN/01 §2）
echo $w->f(2.5), "\n";
