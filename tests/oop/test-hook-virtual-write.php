<?php
// EXPECT: Cannot modify virtual property V::$n without set hook
// EXPECT: ok

// 虚拟属性无 set 钩子 → 写入 Error；可 catch
class V {
    public string $n { get => "v"; }
}
try {
    $v = new V();
    $v->n = "x";
} catch (Error $e) {
    echo $e->getMessage(), "\n";
}
echo "ok\n";
