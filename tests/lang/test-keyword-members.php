<?php
// 关键字可作成员名：PHP 在 -> 之后把关键字退化为标识符
// （$o->fn / $o->match / $o->class，以及同名方法）
class Keywords {
    public $fn = "p-fn";
    public $match = "p-match";
    public $class = "p-class";

    public function fn(): string { return "m-fn"; }
    public function match(): string { return "m-match"; }
}

$k = new Keywords();
echo $k->fn, "\n";
echo $k->match, "\n";
echo $k->class, "\n";
echo $k->fn(), "\n";
echo $k->match(), "\n";

// ?-> 同一规则
$n = null;
var_dump($n?->fn);

// enum case 名同规则（半保留字：case Default / case List 合法，PHP 对齐；
// 2026-11-04 修：enum case 定义位改走 tok_is_member_name）
enum KwFeat: int { case Default = 0; case List = 7; }
echo KwFeat::Default->value, "\n";
echo KwFeat::List->value, "\n";

// EXPECT: p-fn
// EXPECT: p-match
// EXPECT: p-class
// EXPECT: m-fn
// EXPECT: m-match
// EXPECT: NULL
// EXPECT: 0
// EXPECT: 7
