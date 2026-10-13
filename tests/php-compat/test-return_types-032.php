<?php
// from php-src Zend/tests/032.phpt
// EXPECT: DONE
class A {
    function foo(): ?int {}
}
class B extends A {
    function foo(): int {}
}
?>
DONE
