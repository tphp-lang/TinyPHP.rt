<?php
// from php-src Zend/tests/008.phpt
// EXPECT: qux
interface foo {
    public function bar() : foo;
}


class qux implements foo {
    public function bar() : qux {
        return $this;
    }
}

$qux = new qux();
echo get_class($qux->bar());

