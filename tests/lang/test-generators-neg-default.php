<?php
// EXPECT-EXIT: 255
// G3 negative: yield in a default value (const-expr context)
function f($x = yield) {
    return $x;
}
