<?php
// from php-src Zend/tests/009.phpt
// EXPECT: string(3) "bar"
trait Foo {
    public function someMethod() {
      return "bar";
    }
}

$anonClass = new class {
    use Foo;
};

var_dump($anonClass->someMethod());
