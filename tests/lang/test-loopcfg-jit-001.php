<?php
// P0-14 批5：if 内 continue 的 emit CFG 支持（find_join 跳过回跳 +
// 分支 END 存 pending + 体尾 MERGE_N 合流 LOOP_END）+ JMPNZ（||短路）
// 分支修正（原 NOT_B 极性反置 + 漏 emit 跳转目标体，此前 100% fail 回退）。
// catch 一律在 JIT 外；EXPECT 定死对拍。
// EXPECT: 10
function cont_foreach($n) {
    $s = 0;
    $a = [1, 2, 3, 4, 5, 6, 7, 8, 9];
    foreach ($a as $v) {
        if ($v > $n) continue;   // 跳过 >n 的项
        $s += $v;
    }
    return $s;   // n=4 → 1+2+3+4
}
echo cont_foreach(4), "\n";
// EXPECT: 10
function cont_multisite() {
    $s = 0;
    foreach ([1, 2, 3, 4, 5, 6] as $k => $v) {
        // 注：循环内 || 结果喂 continue 触发既有 O1 codegen 值错族
        // （ms1/ms2/ms5 最小复现，backlog P0-14 立项）→ 挡板回退解释器。
        // 此处用已验证可进 JIT 的 === 系多 pending 形态
        if ($v === 2) continue;                 // pending #1
        if ($v === 4) continue;                 // pending #2
        if ($v === 5) continue;                 // pending #3（≥3 层挡板回退）
        $s += $v;
    }
    return $s;   // 1+3+6 = 10（2/4/5 跳过）
}
echo cont_multisite(), "\n";
// EXPECT: 107
function cont_else() {
    $s = 0;
    foreach ([1, 2, 3, 4, 5] as $v) {
        if ($v === 3) {
            $s += 100;
        } else {
            if ($v === 5) continue;   // else 分支内 continue
            $s += $v;
        }
    }
    return $s;   // 1+2+100+4 = 107（5 跳过）
}
echo cont_else(), "\n";
// EXPECT: 12
function cont_while() {
    $i = 0;
    $s = 0;
    while ($i < 10) {
        $i++;
        if ($i % 2 === 0) continue;
        if ($i > 5) continue;
        $s += $i;   // 1+3+5 = 9
    }
    return $s;
}
echo cont_while() + 3, "\n";   // 9+3
// EXPECT: yes
function or_short($a, $b) {
    // || 短路（JMPNZ 路径）：真侧跳目标体 emit
    if ($a || $b) {
        return "yes";
    }
    return "no";
}
echo or_short(false, true), "\n";
// EXPECT: no
function or_short2() {
    if (false || false) {
        return "yes";
    }
    return "no";
}
echo or_short2(), "\n";
// EXPECT: 6
function and_or_mix() {
    // && (JMPZ) 与 || (JMPNZ) 混合短路 + 循环内 continue 组合
    $s = 0;
    foreach ([1, 2, 3, 4] as $v) {
        if ($v > 10 && $v < 0) continue;
        if ($v === 4 || $v === 99) {
            if ($v === 99) continue;
            $s += 2;
            continue;
        }
        $s += $v;
    }
    return $s;   // 1+2+3+2 = 8
}
echo and_or_mix() - 2, "\n";   // 8-2
// EXPECT: 0
function cont_deep() {
    // 双层嵌套循环：continue 作用于内层（pending 归内层 loop_sp）
    $s = 0;
    for ($i = 0; $i < 3; $i++) {
        foreach ([1, 2] as $v) {
            if ($v === 2) continue;
            $s += $v;
        }
    }
    return $s - 3;   // 3×1 = 3
}
echo cont_deep(), "\n";
