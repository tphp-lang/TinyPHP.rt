<?php
// from php-src Zend/tests/__callStatic.phpt
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   string(3) "bar"
// EXPECT:   [1]=>
// EXPECT:   array(2) {
// EXPECT:     [0]=>
// EXPECT:     string(3) "baz"
// EXPECT:     [1]=>
// EXPECT:     string(3) "qux"
// EXPECT:   }
// EXPECT: }
enum Foo {
    public static function __callStatic(string $name, array $args)
    {
        return [$name, $args];
    }
}

var_dump(Foo::bar('baz', 'qux'));

