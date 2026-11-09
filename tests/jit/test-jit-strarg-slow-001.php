<?php
// 刀19 静态直调非 INT 实参慢路（TPHP_CALL_STR_SLOW，默认开）对拍：
// 旧行为（HEAD/关）：JIT'd caller 折叠调用普通函数且实参 tag≠T_INT →
// lower_call_finish INT 契约失配 RETURN(0) → 函数级从 pc0 全量重跑 →
// 序前副作用双执行（s1 旧输出多一个 A；同时是刀18 real-logstat +47% 真凶）。
// 新行为：慢路 tphp_jit_call_userfn（合成 0 捕获闭包 → guarded 同
// op_do_fcall C 侧协议）→ callee 单次执行、序前状态零重跑，与 PHP 逐字一致。
// 覆盖：s1 序前 echo+STR 实参（核心复现）；s2 ARR 实参（slot 型，非 CONST）；
// s3 2000 次 STR 实参 rc 稳定（慢路高频路径无泄漏/双释放）；
// s4 慢路异常逃逸（callee JIT'd 内 raise → ok2=0 → RETURN 上传 → 顶层 catch）。
// 三模语义一致：默认 / TPHP_JIT=0（TPHP_CALL_STR_SLOW=0 为 HEAD 故意坏行为，
// 不入基线，仅 A/B 用）。
// EXPECT: --- strarg-slow ---
// EXPECT: A
// EXPECT: s1=5
// EXPECT: B
// EXPECT: s2=3
// EXPECT: s3=8000
// EXPECT: s4=DivisionByZeroError
// EXPECT: END

echo "--- strarg-slow ---\n";

// s1: 序前副作用 + STR 字面量实参（旧行为：echo 双写）
function ss_h1($s) { return strlen($s); }
function ss_m1() {
    echo "A\n";
    return ss_h1("hello");
}
$r = ss_m1();
echo "s1=$r\n";

// s2: ARR 实参（slot 槽非 CONST，慢路借用读 + callable 内 addref）
function ss_h2($a) { return count($a); }
function ss_m2() {
    echo "B\n";
    return ss_h2([10, 20, 30]);
}
$r = ss_m2();
echo "s2=$r\n";

// s3: 2000 次动态 STR 实参——慢路全量压测（合成闭包 rc + 队列 addref/release 配平）
function ss_h3($s) { return strlen($s); }
function ss_m3() {
    $sum = 0;
    for ($i = 0; $i < 2000; $i++) {
        $v = "abcd";
        $sum += ss_h3($v);
    }
    return $sum;
}
$r = ss_m3();
echo "s3=$r\n";

// s4: 慢路异常逃逸（args 含 STR 走慢路；callee JIT'd 内 DIV0 raise →
// guarded 返回 0+exception → IR RETURN → op_do_fcall longjmp → 顶层 catch）
function ss_h4($s, $d) { return strlen($s) / $d; }
function ss_m4() { return ss_h4("x", 0); }
try {
    ss_m4();
} catch (DivisionByZeroError $e) {
    echo "s4=" . get_class($e) . "\n";
}

echo "END\n";
