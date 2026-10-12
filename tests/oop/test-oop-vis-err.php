<?php
// 可见性错误：外部访问 private 属性（fatal）
// EXPECT: outer: p
// EXPECT: peek: 
// EXPECT-EXIT: 255

class Vault {
    private $secret = "s";
    public $pub = "p";
}

$v = new Vault();
echo "outer: ", $v->pub, "\n";
echo "peek: ", $v->secret, "\n";
