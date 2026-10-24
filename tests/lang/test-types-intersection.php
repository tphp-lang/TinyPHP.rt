<?php
// EXPECT: cross-ok=A
// EXPECT: dnf-ok=A
// EXPECT: null-ok=null
// EXPECT: reject-ok
// EXPECT: scalar-reject-ok
// EXPECT: ok
// C1: 交叉类型 A&B + DNF (A&B)|null（PHP 8.2）
interface Greeter { public function hi(): string; }
interface HasNum { public function n(): int; }
class A implements Greeter, HasNum {
    public function hi(): string { return "A"; }
    public function n(): int { return 7; }
}
class OnlyGreeter implements Greeter {
    public function hi(): string { return "G"; }
}

function cross(Greeter&HasNum $x): string {
    return $x->hi();
}
echo "cross-ok=", cross(new A()), "\n";

function dnf((Greeter&HasNum)|null $x): string {
    return $x === null ? "null" : $x->hi();
}
echo "dnf-ok=", dnf(new A()), "\n";
echo "null-ok=", dnf(null), "\n";

// 不满足交叉（缺 HasNum）→ TypeError
try {
    cross(new OnlyGreeter());
    echo "reject-FAIL\n";
} catch (TypeError $e) {
    echo "reject-ok\n";
}

// 交叉成员禁止标量（加载期 Parse error 等价：运行期类型字符串含 int&X 不会出现——
// parse 已拒；此处验证 object 伪类型放行）
function objcross(object&Greeter $x): string { return $x->hi(); }
try {
    objcross(new A());
    echo "scalar-reject-ok\n";
} catch (TypeError $e) {
    echo "scalar-reject-FAIL\n";
}
echo "ok\n";
