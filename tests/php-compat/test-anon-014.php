<?php
// from php-src Zend/tests/014.phpt
// EXPECT: string(10) "Running..."
trait TaskTrait {
    function run() {
        return 'Running...';
    }
}
$class = new class() {
  use TaskTrait;
};
var_dump($class->run());
