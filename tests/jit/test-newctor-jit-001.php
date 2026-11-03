<?php
// 批2-8 user-ctor 折叠直调回归（规格 memory/2026-11-03 批2-8 节）。
// JIT=0/1 输出必须逐行一致。覆盖：fold 成功路（ctor 属性写）、静态门回放
// （有值 return / 可选参 argc / return $this）、非 INT 实参运行时契约
// mfail、OSR 内 fold raise（半成品不跑 dtor、对象销毁、外 catch）。
// EXPECT: loop=4500
// EXPECT: ret=7
// EXPECT: opt=9
// EXPECT: opt2=4
// EXPECT: self=3
// EXPECT: str=ok
// EXPECT: raised=100/100
// EXPECT: dtor=1003300
// EXPECT: end

// 1) 主目标：ctor 内属性写 + 循环（object bench 同形态，fold 成功路）。
//    两次调用：首调 ic 冻结（defer_ic），次调编译折叠。
class NC1 { public $v; public function __construct($v) { $this->v = $v; } }
function t1() {
    $s = 0;
    for ($i = 0; $i < 1000; $i++) { $o = new NC1($i % 10); $s += $o->v; }
    return $s;
}
t1();
echo "loop=" . t1() . "\n";   // 45 * 100 = 4500

// 2) 有值 return → jit_ctor_ret_val 门 → 回放（返回值丢弃语义）
class NC2 { public $v; public function __construct() { $this->v = 7; return 42; } }
function t2() { $o = new NC2(); return $o->v; }
t2();
echo "ret=" . t2() . "\n";    // 7

// 3) 可选参 argc 门 → 回放（RECV 补默认值）
class NC6 { public $v; public function __construct($v = 9) { $this->v = $v; } }
function t6a() { $o = new NC6(); return $o->v; }
function t6b() { $o = new NC6(4); return $o->v; }
t6a(); t6b();
echo "opt=" . t6a() . "\n";   // 9
echo "opt2=" . t6b() . "\n";  // 4

// 4) return $this → jit_ctor_ret_val 门 → 回放（generic move 会掏空
//    slot0 读点，两端语义分叉 → 保守不折）
class NC5 { public $v; public function __construct($v) { $this->v = $v; return $this; } }
function t5() { $o = new NC5(3); return $o->v; }
t5();
echo "self=" . t5() . "\n";   // 3

// 5) 非 INT 实参 → 运行时契约守卫 → mfail 回放（mk8 编译折叠后生效）
class NC8 { public $v; public function __construct($v) { $this->v = $v; } }
function mk8($v) { return new NC8($v); }
$sv = "";
for ($i = 0; $i < 300; $i++) {
    $w = ($i == 200) ? "ok" : $i;   // 200 > 热阈值：此时 mk8 已编译折叠
    $o = mk8($w);
    if ($i == 200) { $sv = $o->v; }
}
echo "str=$sv\n";              // ok

// 6) OSR/函数级 fold 直调 raise（半成品不跑 dtor、对象销毁、外 catch）。
//    OSR 阈值 = 10000 backedge 且只能在单次长调用内攒够（t3 调满 127 次
//    会先走函数级编译）→ t3 单次 10020 次循环、永不执行的 new 站点让
//    defer_ic 拖住急切编译；暖机 tw3 先编好 NC3 ctor 的 jit_entry →
//    call1 的 backedge#10000 触发 OSR 编译时 cfold 门过 → fold new-ctor
//    [osr]（后续调用经 backedge ic 挂签直进该 chunk）。k==13 的 ctor
//    除零 raise 走 lower_new_ctor_finish ok==0 路（release + DTORED 半
//    成品 + RETURN → 解释器 unwind → 外 catch）；调用满 127 次强制函数
//    级编译 → fold [fn] 同路再覆盖。奇偶交替调用：偶数成功、奇数 raise。
//    计数：成功调用 10020 对象全析构（reassign 10019 + 帧销毁 1）；失败
//    调用在 k=13 即 raise，只有 13 个成功对象（reassign 12 + unwind 收 $z
//    1），半成品误跑 dtor → +100 暴露。暖机对象计数在暖机后清零。
class NC3 {
    public $v;
    public static $d = 0;
    public function __construct($v) {
        if ($v == 13) { $this->v = 1 / ($v - 13); }
        else { $this->v = $v; }
    }
    public function __destruct() { self::$d++; }
}
function tw3($v) { $z = new NC3($v); }   // 暖机：eager 编译连带编 NC3 ctor
for ($i = 0; $i < 5; $i++) tw3(1);
NC3::$d = 0;
function t3($b) {
    for ($k = 0; $k < 10020; $k++) {
        $z = new NC3($b ? $k : $k + 1000);   // 奇数调用在 k=13 处 raise
    }
    if ($b < 0) { $w = new NC3(99); }         // 永不执行站点：急切编译延退
}
$nok = 0; $nx = 0;
for ($i = 0; $i < 200; $i++) {
    try { t3($i & 1); $nok++; }
    catch (Throwable $e) { $nx++; }
}
echo "raised=$nok/$nx\n";      // 100/100
echo "dtor=" . NC3::$d . "\n"; // 100*10020 + 100*13 = 1003300
echo "end\n";
