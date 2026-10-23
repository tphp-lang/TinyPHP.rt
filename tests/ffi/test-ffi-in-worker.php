<?php
// EXPECT: abs=8
// EXPECT: ok

// Worker 线程内 FFI 可用（阻塞调用放 Worker 的文档路径）
use Tphp\Worker;

$w = new Worker(function () {
    $me = Worker::self();
    $ffi = FFI::cdef("int abs(int j);", "msvcrt.dll");
    $me->post("abs=" . $ffi->abs(-8));
});
echo $w->recv(), "\n";
$w->stop();
echo "ok\n";
