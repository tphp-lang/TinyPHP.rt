<?php
// 刀20 p_sm 静态方法直调非 INT 实参慢路（TPHP_CALL_STR_SLOW，默认开）对拍：
// 旧行为（HEAD/gate0）：JIT'd caller 字面量静态方法调用 + STR 实参 → INT 契约
// 失配 RETURN(0) → 函数级 pc0 全量重跑 → 序前 echo 双写（s1 旧输出两个 A）。
// 新行为：慢路 helper 自判 is_method 合成 n_cap=1 垫闭包对位 slot1.. 布局
// （刀9 门放宽 `!is_method || clo` → e() 直入），单次执行与 PHP 逐字一致。
// 覆盖：s1 STR 核心复现；s2 INT 对照（契约快路不变）；s3 2000 次 STR rc 稳定；
// s4 慢路异常逃逸；s5 多参混合（STR+INT+STR）；s6 ARR 实参；
// s7 实例方法（p_mcall mfail 回放路径——锁定不双写）。
// 三模语义一致：默认 / TPHP_JIT=0（gate0=HEAD 故意坏行为，仅 A/B 不入基线）。
// EXPECT: --- sm-strarg ---
// EXPECT: A
// EXPECT: s1=5
// EXPECT: B
// EXPECT: s2=7
// EXPECT: s3=8000
// EXPECT: s4=DivisionByZeroError
// EXPECT: s5=12
// EXPECT: s6=3
// EXPECT: C
// EXPECT: s7=13
// EXPECT: END

echo "--- sm-strarg ---\n";

class SMK {
    public static function len($s) { return strlen($s); }
    public static function num($n) { return $n + 1; }
    public static function div($s, $d) { return strlen($s) / $d; }
    public static function mix($a, $n, $b) { return strlen($a) + $n + strlen($b); }
    public static function cnt($a) { return count($a); }
}

// s1: 序前副作用 + STR 字面量实参（核心复现）
function sm_m1() {
    echo "A\n";
    return SMK::len("hello");
}
$r = sm_m1();
echo "s1=$r\n";

// s2: INT 对照（契约快路，行为不变）
function sm_m2() {
    echo "B\n";
    return SMK::num(6);
}
$r = sm_m2();
echo "s2=$r\n";

// s3: 2000 次动态 STR 实参——慢路高频压测（垫闭包 rc + 队列配平）
function sm_m3() {
    $sum = 0;
    for ($i = 0; $i < 2000; $i++) {
        $v = "abcd";
        $sum += SMK::len($v);
    }
    return $sum;
}
$r = sm_m3();
echo "s3=$r\n";

// s4: 慢路异常逃逸（STR 走慢路；callee JIT'd 内 DIV0 → guarded 返 0+exception
// → IR RETURN → op_do_fcall longjmp → 顶层 catch）
try {
    sm_m4();
} catch (DivisionByZeroError $e) {
    echo "s4=" . get_class($e) . "\n";
}
function sm_m4() { return SMK::div("x", 0); }

// s5: 多参混合 STR+INT+STR（首参即失配 → 整组走慢路，args 全量搬运）
function sm_m5() { return SMK::mix("abc", 5, "defg"); }
echo "s5=" . sm_m5() . "\n";

// s6: ARR 实参
function sm_m6() { return SMK::cnt([1, 2, 3]); }
echo "s6=" . sm_m6() . "\n";

// s7: 实例方法 STR 实参（p_mcall mfail 回放路径——既往已修，锁双写回归）
class IMK {
    public $base = 8;
    public function addlen($s) { return $this->base + strlen($s); }
}
function sm_m7() {
    echo "C\n";
    $o = new IMK();
    return $o->addlen("abcde");
}
$r = sm_m7();
echo "s7=$r\n";

echo "END\n";
