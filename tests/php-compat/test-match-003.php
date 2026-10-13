<?php
// from php-src Zend/tests/003.phpt
// EXPECT: default
// EXPECT: 1
// EXPECT: 2
// EXPECT: default
function get_value($i) {
    return match ($i) {
        1 => 1,
        2 => 2,
        default => 'default',
    };
}

echo get_value(0) . "\n";
echo get_value(1) . "\n";
echo get_value(2) . "\n";
echo get_value(3) . "\n";

