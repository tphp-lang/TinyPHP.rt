<?php
// B6: 归因 quirk——调用点不在 try 内、其后紧跟 try 时，callee 的异常不得
// 误入下一个 try 保护区（PHP: uncaught fatal，与 try 间插语句行为一致）
// EXPECT-EXIT: 255
function boom6() { throw new RuntimeException("q"); }
boom6();
try { echo "try body\n"; } catch (Throwable $e) { echo "WRONG caught: ", $e->getMessage(), "\n"; }
echo "end\n";
