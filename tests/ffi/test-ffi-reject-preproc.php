<?php
// EXPECT-EXIT: 255

// 预处理指令必须拒绝
FFI::cdef("#include <stdio.h>\nint foo(void);");
echo "should not reach\n";
