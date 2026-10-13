<?php
// from php-src Zend/tests/022.phpt
// EXPECT: string(3) "bar"
$foo = "bar";
$test = function() use($foo) : Closure {
    return function() use ($foo) {
        return $foo;
    };
};

$callable = $test();
var_dump($callable());
