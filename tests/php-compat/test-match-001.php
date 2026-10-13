<?php
// from php-src Zend/tests/001.phpt
// EXPECT: Zero
// EXPECT: One
// EXPECT: Two
// EXPECT: Three
// EXPECT: Four
// EXPECT: Five
// EXPECT: Six
// EXPECT: Seven
// EXPECT: Eight
// EXPECT: Nine
function wordify($x) {
    return match ($x) {
        0 => 'Zero',
        1 => 'One',
        2 => 'Two',
        3 => 'Three',
        4 => 'Four',
        5 => 'Five',
        6 => 'Six',
        7 => 'Seven',
        8 => 'Eight',
        9 => 'Nine',
    };
}

for ($i = 0; $i <= 9; $i++) {
    print wordify($i) . "\n";
}

