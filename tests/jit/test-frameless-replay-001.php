<?php
// 无帧直调 × replay 体回归网（2026-11-02，opseq 自框化 temp VMFrame 修复）：
// JIT→JIT 直调是裸 entry(vm, slots, ret)（无 VMFrame 压入），而 tphp_jit_opseq
// 以 vm->x.frame 为回放帧——callee 体含 replay op（new/prop/foreach pad）时，
// 无帧入口会拿调用方帧按 callee pc 索引 ops → 乱执行 → 0xC0000005 /
// undefined temporary。批2-4 只在 INIT_MCALL 折叠门挡了 jit_body_frameless，
// INIT_FCALL 静态直调 / p_dyn 链 / call_slots 无守卫。修复：opseq 入口检测
// fn/slots 与现帧不符即自框化（解释器/OSR 入口零成本跳过）。
// 覆盖：静态直调、方法调用直调、递归（同 fn 不同 slots 同样触发）、
// 逃逸路径（replay 中 raise → temp 帧清理 + 异常传播）。
// TPHP_JIT=0/1 必须产生相同输出。
// EXPECT: r1=19999900000
// EXPECT: r2=5050
// EXPECT: r3=150
// EXPECT: r4=caught
// EXPECT: r5=5050

// 1) 静态直调 × callee 体含 new + ASSIGN_PROP（replay op）——最小崩溃形
class FrC { public $v; }
function fr_new($i) { $q = new FrC(); $q->v = $i; return $q->v; }
$p = 0;
for ($i = 0; $i < 200000; $i++) $p += fr_new($i);
echo "r1=$p\n";

// 2) 递归直调：同 fn 但 slots 不同（scratch）——同样自框化。
//    深度须 < VM_MAX_CALLS(256)：解释器在 call_push raise Call stack
//    overflow；JIT 直调无该守卫（HEAD 既有，纯标量递归同样段错误），
//    深度 100 同时落在两者安全区。
function fr_rec($n) {
    if ($n <= 0) return 0;
    $q = new FrC();
    $q->v = $n;
    return $q->v + fr_rec($n - 1);
}
echo "r2=" . fr_rec(100) . "\n";

// 3) 方法直调（INIT_MCALL 折叠路）× callee 体含 prop replay
class FrM {
    public $v = 3;
    public function hit($i) { $q = new FrC(); $q->v = $i; return $q->v + $this->v; }
}
$fm = new FrM();
$s3 = 0;
for ($i = 0; $i < 15; $i++) $s3 += $fm->hit($i);
echo "r3=$s3\n";

// 4) 逃逸路径：replay 中 raise → opseq 返 0 清理 temp 帧 → 异常传播到 catch
function fr_throw($i) {
    $q = new FrC();
    $q->v = $i;
    if ($i == 100) throw new Exception("boom");
    return $q->v;
}
$s4 = "ok";
try {
    $t = 0;
    for ($i = 0; $i < 20000; $i++) $t += fr_throw($i);
} catch (Exception $e) {
    $s4 = "caught";
}
echo "r4=$s4\n";

// 5) 对照：纯标量直调（无 replay）保持原路
function fr_add($i) { return $i + 1; }
$s5 = 0;
for ($i = 0; $i < 100; $i++) $s5 += fr_add($i);
echo "r5=$s5\n";
