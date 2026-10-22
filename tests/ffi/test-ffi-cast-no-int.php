<?php
// EXPECT: reject-int
// EXPECT: null-ok
// EXPECT: cdata-ok
// EXPECT: addr-roundtrip-ok

// FFI::cast 收紧（ffi-security §8）：拒裸 int，只接受 CData / null
$ffi = FFI::cdef("void free(void *p);");

// 裸 int → TypeError（整数→指针任意地址读写总入口）
$rejected = false;
try {
    FFI::cast("char*", 42);
} catch (TypeError $e) {
    $rejected = true;
}
echo $rejected ? "reject-int\n" : "reject-int-FAIL\n";

// null → NULL 指针，合法
$n = FFI::cast("char*", null);
echo "null-ok\n";

// 已有 CData → 重解释，合法
$buf = FFI::new("int[4]");
$p = FFI::cast("char*", $buf);
echo "cdata-ok\n";

// 指针 → 整数（getAddress）方向无害，roundtrip 可用
$addr = $p->getAddress();
$q = FFI::cast("char*", $p);   // CData 再 cast 合法
$ok = ($q->getAddress() === $addr);
echo $ok ? "addr-roundtrip-ok\n" : "addr-roundtrip-FAIL\n";
