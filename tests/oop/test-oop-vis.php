<?php
// 可见性：类内互访、protected 子类链、外部仅 public、静态属性可见性
// EXPECT: inner: secret, prot, pub
// EXPECT: child: prot, pub
// EXPECT: outer: pub
// EXPECT: sprop: s1,s2
// EXPECT: sprop-outer: pub-s

class Vault {
    private $secret = "secret";
    protected $prot = "prot";
    public $pub = "pub";
    private static $s1 = "s1";
    protected static $s2 = "s2";
    public static $s3 = "pub-s";

    public function inner() {
        return "inner: {$this->secret}, {$this->prot}, {$this->pub}";
    }
    public function sprop() {
        return "sprop: " . self::$s1 . "," . self::$s2;
    }
}

class VaultChild extends Vault {
    public function child() {
        return "child: {$this->prot}, {$this->pub}";
    }
    public function childSprop() {
        return "cs: " . parent::$s2;
    }
}

$v = new Vault();
echo $v->inner(), "\n";
$c = new VaultChild();
echo $c->child(), "\n";
echo "outer: ", $v->pub, "\n";
echo $v->sprop(), "\n";
echo "sprop-outer: ", Vault::$s3, "\n";
