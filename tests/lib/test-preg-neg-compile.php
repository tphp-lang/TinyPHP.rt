<?php
// EXPECT-EXIT: 255
// P4 negative: compilation failure (unbalanced group)
preg_match("/(unclosed/", "x");
