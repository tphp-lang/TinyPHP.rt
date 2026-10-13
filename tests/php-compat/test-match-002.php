<?php
// from php-src Zend/tests/002.phpt
// EXPECT: true
// EXPECT: false
function print_bool($bool) {
    echo match ($bool) {
        true => "true\n",
        false => "false\n"
    };
}

print_bool(true);
print_bool(false);

