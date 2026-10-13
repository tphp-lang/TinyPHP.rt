<?php
// from php-src Zend/tests/__call.phpt
// EXPECT: array(2) {
// EXPECT:   [0]=>
// EXPECT:   string(3) "baz"
// EXPECT:   [1]=>
// EXPECT:   array(2) {
// EXPECT:     [0]=>
// EXPECT:     string(3) "qux"
// EXPECT:     [1]=>
// EXPECT:     string(4) "quux"
// EXPECT:   }
// EXPECT: }
enum Foo {
    case Bar;

    public function __call(string $name, array $args)
    {
        return [$name, $args];
    }
}

var_dump(Foo::Bar->baz('qux', 'quux'));

