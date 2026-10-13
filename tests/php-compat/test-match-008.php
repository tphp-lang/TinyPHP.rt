<?php
// from php-src Zend/tests/008.phpt
// EXPECT: bool(false)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(false)
function is_working_day($day) {
    return match ($day) {
        1, 7 => false,
        2, 3, 4, 5, 6 => true,
    };
}

for ($i = 1; $i <= 7; $i++) {
    var_dump(is_working_day($i));
}

