<?php
// from php-src Zend/tests/002.phpt
// EXPECT: bool(true)
// EXPECT: bool(true)
class A{}

interface B{
    public function method();
}

$a = new class extends A implements B {
    public function method(){
        return true;
    }
};

var_dump($a instanceof A, $a instanceof B);
