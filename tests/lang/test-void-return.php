<?php
// B1: void 函数带值 return = 加载期拒绝（对齐 PHP Fatal，无可执行代码）
// EXPECT-EXIT: 255
function v(): void { return 5; }
