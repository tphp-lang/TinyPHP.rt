<?php
// EXPECT-EXIT: 255
// G3 negative: yield outside a function (load-time error)
yield 1;
