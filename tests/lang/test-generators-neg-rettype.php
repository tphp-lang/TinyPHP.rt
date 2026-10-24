<?php
// EXPECT-EXIT: 255
// G3 negative: generator must not declare a non-Generator return type
function g(): int {
    yield 1;
}
