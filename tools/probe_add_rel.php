<?php
// 刀2 探针：JIT ADD 快线是否释放旧 dst（对照 = TPHP_JIT=0 解释器）
class D {
    static $n = 0;
    function __destruct() { D::$n++; }
}
function f() {
    $x = new D();       // 旧值 = refcounted 对象
    $x = 1 + 2;         // ADD 快线 store 覆盖 CV——若不 release 则泄漏，dtor 缺失
    return $x;
}
for ($i = 0; $i < 300; $i++) $r = f();
echo "dtors=", D::$n, " r=$r\n";
