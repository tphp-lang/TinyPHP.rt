<?php
// from php-src Zend/tests/011.phpt
// EXPECT: string(3) "foo"
// EXPECT: string(3) "foo"
function dump_and_return($string) {
    var_dump($string);
    return $string;
}

var_dump(match ('foo') {
    'foo' => dump_and_return('foo'),
    'bar' => dump_and_return('bar'),
});

