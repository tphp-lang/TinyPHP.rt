<?php
// EXPECT-EXIT: 255
// P4 negative: unknown modifier
preg_match("/a/Zz", "x");
