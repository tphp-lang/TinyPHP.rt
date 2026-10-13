<?php
// EXPECT: owned_free_ok
// EXPECT: gc_ok
// EXPECT: ok

// 显式 FFI::free
$p = FFI::new("int[4]");
FFI::free($p);
echo "owned_free_ok\n";

// owned CData 交给 GC（native_dtor 释放缓冲）——不崩溃即通过
for ($i = 0; $i < 100; $i++) {
    $x = FFI::new("int[16]");
    $x[0] = $i;
}
gc_collect_cycles();
echo "gc_ok\n";
echo "ok\n";
