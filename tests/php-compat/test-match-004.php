<?php
// from php-src Zend/tests/004.phpt
// EXPECT: 20-30
// EXPECT: 0-10
// EXPECT: 50+
// EXPECT: 10-20
// EXPECT: 30-40
// EXPECT: 40-50
function get_range($i) {
    return match (true) {
        $i >= 50 => '50+',
        $i >= 40 => '40-50',
        $i >= 30 => '30-40',
        $i >= 20 => '20-30',
        $i >= 10 => '10-20',
        default => '0-10',
    };
}

echo get_range(22) . "\n";
echo get_range(0) . "\n";
echo get_range(59) . "\n";
echo get_range(13) . "\n";
echo get_range(39) . "\n";
echo get_range(40) . "\n";

