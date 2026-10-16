<?php
// B1: never 函数带值 return = 加载期拒绝（对齐 PHP Fatal）
// EXPECT-EXIT: 255
function n(): never { return 5; }
