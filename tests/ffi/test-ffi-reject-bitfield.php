<?php
// EXPECT-EXIT: 255

// bitfield 必须明确拒绝
FFI::cdef("struct B { unsigned int a : 3; unsigned int b : 5; };");
echo "should not reach\n";
