<?php
// EXPECT: param-distinct=yes
// EXPECT: param-val=param
// EXPECT: attr-distinct=yes
// EXPECT: attr-val=attr
// EXPECT: const-val=const
// EXPECT: promoted-ok=yes
// EXPECT: ok
// C3: new in initializers（PHP 8.1）——每次求值点新建实例
// 历史 bug：thunk 嵌套循环旧语义让 ctor 帧成孤儿（$this=null）——已修（deep 语义）
class Holder {
    public $v;
    public function __construct($v = null) { $this->v = $v; }
}
class WithAttr {
    public $h = new Holder("attr");
}
class K {
    const H = new Holder("const");
}
class Promoted {
    public function __construct(public $p = new Holder("promoted")) {}
}
function make($o = new Holder("param")) {
    return $o;
}

$a = make();
$b = make();
echo "param-distinct=", ($a !== $b) ? "yes" : "no", "\n";
echo "param-val=", $a->v, "\n";

$w1 = new WithAttr();
$w2 = new WithAttr();
echo "attr-distinct=", ($w1->h !== $w2->h) ? "yes" : "no", "\n";
echo "attr-val=", $w1->h->v, "\n";

echo "const-val=", K::H->v, "\n";

$p1 = new Promoted();
$p2 = new Promoted();
echo "promoted-ok=", ($p1->p !== $p2->p && $p1->p->v === "promoted") ? "yes" : "no", "\n";
echo "ok\n";
