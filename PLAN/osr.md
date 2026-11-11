# PLAN/osr.md · 循环 OSR 规格（解释器热循环 → IR baseline JIT）

> 状态：**批 1 已落地**（2026-10-31：back-edge 计数 + 单循环 OSR，`tests/jit/test-osr-001.php` 6 用例对拍全对，234/234，20M 迭代 int 循环 2.6-2.8×；批 2 未开工）。
> 上级索引：[../PLAN.md](../PLAN.md)。JSC 借鉴分析：[jsc-lessons](jsc-lessons.md) §4。函数级 JIT 规格：[ir-jit](ir-jit.md)（v1.2 direct-slot ABI）。
> 定位：**M3 敏感簇反超主力组合**的最后一块（循环 OSR + P0-13 IC + P0-14 profile 准入）。

## 1. 动机与定位

函数级准入的盲区：**函数含任一白名单外 op 即整函数拒绝**——而敏感簇 call/read/kv 的循环体恰好常伴白名单外 op（bench 闭包的 `$m[$keys[$k]]` 外围有字符串校验/数组初始化；真实脚本的循环体外几乎总有杂 op）。当前这些热循环**永远解释器**（A/B 实证：bench 闭包单次调用 + 无 OSR → 长循环恒解释器，IC 收益全在 C 侧）。

OSR 编译单元 = **自然循环子图**（loop header → back-edge），白名单判定应用于循环体 op 集——循环外的杂 op 不参与编译，循环内仍是封闭白名单世界。

**不改变准入语义**：OSR 不是放宽白名单，是把编译单元从"函数"缩小到"循环"。

## 2. tphp 架构红利（为什么比 JSC 简单一个量级）

| 环节 | JSC（DFGOSREntry.cpp 416 行） | tphp |
|---|---|---|
| 帧状态映射 | LLInt 寄存器化栈帧 ↔ JIT 帧布局**不同**，需 availability analysis + 逐 slot 物化 | **VMFrame slots 与 JIT slots 布局完全一致**（ir-jit §3：`[captures][this?][params][locals][tmps]`）→ OSR entry **零拷贝直传 `frame->slots`** |
| live-in 求值 | DFG availability analysis 相位 | **无需分析**：IR lowering 一律从 slots 内存 load（不做跨入口寄存器假设），live-in 天然就位 |
| 编译入口 | 独立 OSR 版编译管线 | `emit_seq` 已支持任意 pc 起 emit（P0-14 lower_replay 在用）；函数级 JIT 已支持体内循环（`trap_lb` 回边补丁） |
| exit 恢复 | DFGOSRExit.cpp 921 行逐 slot 恢复 | P0-12 deopt 框架已闭环（JIT→解释器状态恢复），复用 |

**结论：tphp 的 OSR = 现有函数级 JIT 的"第二个入口点"，不是第二条编译管线。**

## 3. 核心设计

### 3.1 触发：back-edge 计数（解释器侧）

- 判定点：`op_jmp` 中 `target <= pc`（向后跳 = back-edge；JMPZ/JMPNZ 向后跳在 JIT 侧是 trap_fail 形态，不做计数点）；
- 计数器：**`TOp.ic` 槽复用**（JMP 类 op 无 DimIC 占用，无冲突），**per-site** 三态编码（落地版，替代原"每函数最热 1 循环"设计——见 §3.2）：
  `0..THRESHOLD-1` = 计数器；bit63 tag = 该位点 OSR chunk 入口（用户态代码指针 bit63 恒 0）；`UINTPTR_MAX` = 熔断；
- **阈值**：`TPHP_JIT_OSR_THRESHOLD`，默认 **10000**（back-edge 次）——保守，理由：OSR 编译一次 ~10-85ms 塞进循环中间，阈值过低对短命循环纯亏（`n_ops × 编译成本/迭代收益` 盈亏平衡约 1e4 量级迭代）；函数级阈值 127 不适用（back-edge 每迭代一次）；
- 成本：每 back-edge `load ++ cmp store` ≈ 1-2ns，占 dispatch 基线（67ns/轮）<3%；
- **不递归计数**：内层循环 back-edge 只计内层；嵌套循环外层循环体内含内层 back-edge 跳转不误触（JMP 目标判定天然按 site 归属）。

### 3.2 编译单元：单自然循环（v1 范围）

- 循环识别（编译期，静态）：header = back-edge 目标 pc；循环体 = header → back-edge 的可达 op 区间（v1 用**线性区间近似**：`[header, back_edge_pc]` 内、且跳转目标不落在区间外的 op——多出口循环的额外出口视为 exit）；
- **per-site 独立编译**（落地版，替代原"每函数最热 1 循环"）：每个 back-edge site 的 `TOp.ic` 自行计数/存自己的 chunk 入口（bit63 tag）/自行熔断。原设计（`TFunc` 增 `osr_entry/osr_header/osr_exit` 字段、每函数只编最热 1 循环）**已废弃**——实测踩坑：函数级入口被同函数第二个热循环复用，A 循环的 chunk 读 B 循环的槽，cond 常假立即返回 A 的出口 pc，A/B 间 ping-pong（4000 万圈，值全对但 B 永远到不了）；per-site 后每个循环独立计数独立编译，`TFunc` 零新增字段；
- 白名单：循环体 op 集 = ir-jit §6 白名单 + P0 系列已落地扩展（IC 内联链 / prop 回放 / CONCAT helper 等，与函数级 eligibility **共用同一判定函数**，传 pc 区间参数）。批 1 白名单见 §7 批 1 落地记录。

### 3.3 OSR entry ABI

```c
/* OSR 版循环体；slots = 解释器当前帧（零拷贝，直读直写）。
 * 返回值 = resume pc（解释器应从该 op 继续）：正常 exit = 出口 op 的 pc；
 * deopt/fail 点 = 失败 op 自己的 pc（解释器重跑该 op，批 1 白名单幂等）。 */
typedef int (*OsrEntry)(VM *vm, TVal *slots);
```

- IR 结构：`START → PARAM(vm=1/slots=2) → emit_seq(header, stops=exits)`——与函数级唯一区别：**入口 pc = header**、prologue 不做任何槽初始化（解释器帧现成）；所有 fail/deopt 点 `ir_RETURN(ir_CONST_I32(cur_pc))`（OSR_RET_PC 宏，函数级同点位返 0）；
- **终端 END 收尾（落地时踩坑）**：`emit_seq` 命中 stop 返回 `TR_END(end, target)`——TR_END 仅 OSR 出现（函数级 stops=NULL 永远 TR_RET/FAIL/LOOPBACK），出口块必须 `ir_BEGIN(t.end) + ir_RETURN(t.target)` 终止，否则 codegen 落出代码缓冲（实测：垃圾 resume_pc → VM 跳飞挂死；ir_check 不查尾块 terminator）；
- 循环回边：现有 `trap_lb` 机制原样复用；
- **JIT→JIT 调用**（循环体内 do_fcall 直调 callee）：direct-slot 机制原样复用（callee slots 为 ALLOCA，与帧 slots 无关）——调用语义零新增；
- code 管理：**per-site 挂 back-edge op 的 `TOp.ic`**（bit63 tag，一次写全局只读；线程模型同 §10 ir-jit）。

### 3.4 exit 与 deopt（复用 P0-12）

- **正常 exit**：循环条件不成立走主 exit path → `*resume_pc = osr_exit` → 解释器从循环后继续（循环体内已写的 slots 就是解释器状态——同布局免恢复）；
- **deopt**：体内 helper fail / IC 链 miss 回退 / raise 不可能（封闭世界）——fail 点的**解释器 pc = 编译期已知**（emit 时记 op→pc 映射，helper 返回路径携带）→ 解释器从该 pc 重跑该 op（op 幂等性由现有 deopt 语义保证，P0-12 已验证同款）；
- **协程/异常**：封闭世界无挂起点、无 raise——与函数级 JIT 同结论，无新增面。

### 3.5 编译停顿治理

OSR 编译同步塞在循环中间（解释器循环跑到 1 万次时停 10-85ms）。v1 治理：

1. **保守阈值 10000**（§3.1）；
2. `n_ops ≤ 96`（急切编译同款上限，超限不编——大循环体编译成本失控风险）；
3. **P2-13 异步编译**（后台线程，ir-jit §12.3 / jsc-lessons §5）作为批 3 联动项——v1 同步编译 + 保守阈值兜底；
4. 编译失败熔断：`TOp.ic = UINTPTR_MAX` 永不再试（同函数级 `hot=UINT16_MAX` 语义）。

### 3.6 与函数级 JIT 的协同

函数已 JIT'd（jit_entry 有值）→ 解释器不执行其循环 → back-edge 计数不增长（触发点在解释器 handler 内，天然互斥）；OSR 版函数不设函数级 jit_entry（两者独立、可共存）。

## 4. 实施批次

| 批 | 内容 | 验收 |
|---|---|---|
| 批 1 ✅ | back-edge 计数基建 + 单循环 OSR（int 域封闭子集，白名单见 §7） | ✅ 2026-10-31：`tests/jit/test-osr-001.php` 6 用例（for/while/break+continue/INC/嵌套/while+if）TPHP_JIT=0/1 对拍全对；234/234；20M 迭代 int 循环 **2.6-2.8×**（2140→803ms） |
| 批 2-1 ✅ | 循环体白名单加 **FETCH_DIM_R**（读操作幂等，deopt 重跑安全；形态守卫对齐函数级 eligibility） | ✅ 2026-10-31：`test-osr-002.php`（kv 双循环嵌套 dim/packed 直取/tmp 键）+ `test-osr-003.php`（undefined key/non-array/packed overrun 的 raise→deopt→解释器重跑边界，n_ex>0 拒编路径）对拍全对；**236/236**；顶层 kv 形态 195→**58ms（3.4×）**；归因修正见 §7 |
| 批 2 | 循环体白名单扩到 IC 内联链（FETCH_DIM_R）/prop 回放/do_fcall 直调 | bench-vs-php **kv-read / array-read / real-logstat 进 JIT 形态**（今天全是解释器）；敏感簇数字 |
| 批 3 | 多循环表 + P2-13 异步编译联动 + 阈值自调 | 编译停顿 <1ms 体感；M3 组合收官（多循环已随 per-site 落地，批 3 余异步编译+阈值自调） |

## 5. 测试与验收（M3 判据）

- `tests/jit/test-osr-*.php`：①正确性——单层/嵌套循环、循环内 break/continue、循环变量 int 溢出提升、循环体含调用、TPHP_JIT=0/1 对拍；②触发——计数精确性（差一次不触发）；③熔断——失败循环不反复编译；④deopt——循环中 deopt 后解释器续跑结果正确（含 deopt 后循环退出继续函数尾）；
- 性能：`tools/perf-compare.ps1` 对比表——批 2 目标 kv-read / array-read 相对今天（恒解释器形态）显著下降；M3 = 敏感簇 ≤1.0×（与 P0-13/P0-14 合力）。

## 6. 风险与不做清单（防翻案）

| 项 | 决策 |
|---|---|
| 多态/投机类型（JSC DFG 式 speculation） | **不做**——tphp 是 baseline 档，白名单静态封闭；类型投机属 P0-17③ 类型特化，另立项 |
| OSR exit 的反向（JIT→解释器逐 op 精确恢复中间状态） | v1 只需 resume_pc + slots 同布局；不做 DFGOSRExit 921 行级逐 slot 物化（tphp 无需） |
| 循环体含 raise/generator/（历史）FFI | 不做——封闭世界白名单排除，同 ir-jit §13 |
| 编译停顿 | v1 同步 + 阈值 10000 + n_ops≤96；异步化归 P2-13 批 3 |
| on-stack 值替换/逃逸分析 | P0-9/P0-17② 域，不混入 |

## 7. 批 1 落地记录（2026-10-31）

- **白名单**（`osr_body_ok`，比规格 §4 批 1 行更宽但仍封闭）：CONST/COPY/ASSIGN/ADD/SUB/MUL/NEG/POS/INC/DEC/BW_*/SHL/SHR/BOOL_*/CAST/EQ/NE/SEQ/SNE/LT/LE/CMP/ISSET_CV/UNSET_CV/JMP/JMPZ/JMPNZ；CALL/DIM/ARRAY/RETURN/FE/EXTRACT 一律拒；
- **拒编形态实测**：循环体内 `break` → loop-cond 处理器"body must back-edge or fail"→ 熔断回解释器（test-osr-001 case 3 走此路，值正确）；`while(1)` 无条件回边 → TR_LOOPBACK 顶层冒泡拒编；
- **两个落地 bug**（均为首次运行暴露、已修）：
  1. 终端 END 块无 terminator（§3.3）→ 垃圾 resume_pc → VM 跳飞挂死；
  2. 函数级 osr_entry 复用 → 跨循环 ping-pong（§3.2）→ 改 per-site `TOp.ic` bit63 tag 编码；
- **门槛**：`is_main` 允许（OSR 不走函数入口 ABI，顶层循环正是核心场景）；仍拒 is_global 别名槽/variadic/generator/异常表/n_cap>0/typed params；
- **触发链**：`op_jmp`（`ar <= cur`）→ `tphp_osr_backedge`（计数→编译→bit63 tag 直进）→ `tphp_jit_compile_osr` 返回 entry（NULL 即熔断）。

### 批 2-1 落地记录（2026-10-31，FETCH_DIM_R）

- **改动最小化**：仅 `osr_body_ok` 加 `OP_FETCH_DIM_R`（+ t2≠OPD_UNUSED 形态守卫对齐函数级 eligibility）——emit 复用 emit_seq 的函数级 case（packed 直取 + DimIC 链全内联），零新 emit 代码；
- **语义安全论证**：读操作幂等（deopt 重跑安全）；helper raise_guarded 不穿 IR（P0-12），异常由解释器重跑该 op 时抛出，无重复副作用；IC 懒分配 emit 期单次（触发线程即执行线程，同步编译无并发执行）；dimic_take 先 addref 后 release，重跑时 dst 旧值同源，引用计数自平衡；
- **n_ex 门槛注意**：main/函数里**任何 try/catch → n_ex>0 → 整函数拒编**（既有保守门）——test-osr-003 即走此路（拒编安全回退 + 异常语义对拍）。批 2 后续如要"循环体内 try + 循环体白名单内"形态，需 per-loop 异常区间检查，暂不立项；
- **归因修正（批 C 定性更正，不影响当时决策）**：bench 闭包形态（kv-read/array-read）**在批 1 前已被函数级急切编译**（闭包 n_ops≤96），循环在函数级 JIT 里执行；批 C 的 avg_scan=4.48 插桩来自 **IC 链 miss → v1 helper 内部的 tarr_get_ic**，非纯解释器执行。因此 harness 的 kv-read 4.2×（195→46）非 OSR 增量（批 C 时已 ~47ms）；**OSR 批 2-1 的真实增量场景 = main 顶层循环 / n_ops>96 大函数内的循环**（函数级够不着的形态）：顶层 kv 形态 195→**58ms（3.4×）**，DEBUG 实锤 0 次函数级 compile、纯 OSR 贡献；
- **real-logstat harness 顺序效应（与 OSR 无关，记录）**：完整 harness JIT=1 下 real-logstat 82-95ms vs JIT=0 43-54ms；但单跑该形态 64 vs 66ms 零回退，且 harness 全程 0 次 osr compile——归因前序项函数级 JIT 化后的内存/缓存状态连带 + 本机噪声（±10-20%）。验收以单项形态为准，CI（perf.yml）机器态干净待复核。

### 批 2-2 落地记录（2026-10-31，折叠 CALL 序 + prop ops）

- **语义前提（关键论证）**：`tphp_jit_opseq` 返 0 **恒等于 exception**（vm.c setjmp 唯一出口，prop handler/builtin 错误全走 raise）→ 异常传播不重跑，prop 单 op pad（lower_replay）的 deopt resume=cur_pc 恒安全；
- **折叠 CALL 的 resume 语义**：直调 guard fail（entry NULL / INT 契约 / callee deopt 未执行 / call_dyn ok=0）发生时 **INIT..SEND..DO 序未完整执行**——单独重跑 DO 语义断裂（args 栈/rec 缺 INIT）→ 新宏 `OSR_SEQ_PC(j)`（osr ? p_seq_pc : 0）：**从序起点重跑整序幂等**（SEND_VAL/SEND_VAR 源恒为槽；args 表达式在 seq 之外求值，不在重跑范围）；函数级=0 行为不变。**INIT_FCALL/INIT_DYN emit case 补设 `j->p_seq_pc`**（此前只有 INIT_NEW/INIT_MCALL 设——函数级不需要，OSR 需要）；
- **坑 1（c4 段错误实锤）**：OSR 编译路径漏分配 `j.r_call_slots`——B 直调链读它得 ref 0 → **ir_check=0 照常出码 → 崩**（P0-13"ir_check 不 gate"铁律再犯；O0 下 ir_jit_compile 返 NULL 熔断反而安全）。修复 = OSR 路径补 ALLOCA（函数级 2384 行同款）；
- **坑 2（测试构造）**：main 含任何 try/catch → n_ex>0 → main 全部循环拒 OSR（批 1 静态守卫）——异常传播 case 用"循环函数无 try + 顶层壳函数 catch"构造；osr_body_ok 补拒因日志（`[jit] osr reject: pc=N op=X`，对齐 eligibility 审计模式）；
- **白名单**：osr_body_ok +9 op——prop 四 op（FETCH/ASSIGN/ISSET/UNSET_PROP）+ INIT_FCALL（用户 callee 递归准入/builtin ic|1）/SEND_VAL/SEND_VAR（const 源限标量串）/DO_FCALL/INIT_DYN；
- **实测**：test-osr-004.php 6 场景三方一致（PHP 8.5 / JIT=0 / JIT=1）——c1 用户直调、c2 builtin、c3 prop 读写、c4 动态调用（捕获闭包→call_dyn trampoline）、c5 callee mid-loop raise（raises=1 证明无 deopt 重放）、c6 表达式实参；5 个 OSR chunk 全编译（header=2/23/47/78/124，ir_check 全 1）；**238/238**、bench all-ok；
- **性能**：OSR×CALL 基准（main 顶层 20M 次调用 b1($i&255)，OSR 独占场景）3720→**1650ms（2.25×）**，PHP 8.5 JIT 1010ms（0.61×）；
- **遗留（批 2-3 候选）**：INIT_MCALL/NEW_USER 方法与对象链循环形态（opseq replay 同构，白名单一行）；UNSET_CV 已在批 1 白名单；FE_RESET/FE_FETCH 循环形态。### 批 2-3 尝试与撤回记录（2026-10-31，INIT_MCALL/INIT_NEW/NEW_USER）

- **尝试**：osr_body_ok 加方法调用/对象链三 op（opseq replay 同构，fail=exception 论证同批 2-2a）——**c1 方法调用循环编译+执行全对**，c2（new+prop+COPY 组合）**段错误**；
- **根因定位（三态实验矩阵）**：
  - O1+FOLDING：**出坏码**——`tv_copy(dst, src=0)` 段错误（ra 定位在 chunk 内 COPY 调用点，src 的 slot 地址被 CSE 寄存器化后跨 MERGE 分支失效）；ir_check=1 照常出码（铁律再证）；
  - O1 无 FOLDING：**O1 拒出码**（ir_check=1 但 ir_jit_compile 返 NULL size=0 → 熔断）；
  - O0（±FOLDING）：**全对但零收益**——OSR×CALL 基准 3790 vs 解释器 3778ms 持平，且全局 O0 拖累批 2-2 成果（1650→3420ms）。
- **与批 5 同族**：P0-14 批 5 的 foreach 形态同症状（tv_copy src=0、O0 全对），当时根修 = `3470e7e`（IR_BITSET_ONE LLP64 位宽）——**本次是 deps/ir 的另一处**（O1 FOLDING/CSE × LOOP×opseq-replay-MERGE 结构；批 1 注释"skip SCCP — multi-slot CALL bug in deps/ir ir_sccp"同域）；
- **撤回决定**：三 op 撤出 osr_body_ok（MCALL/NEW 循环形态熔断走解释器 = 批 2-2 前行为），O1+FOLDING 恢复，test-osr-005 删除（形态不支持，场景记录在案）；撤回后 238/238、bench all-ok、OSR×CALL 1650ms 恢复 ✓；
- **重开条件**：deps/ir 根修 O1 FOLDING 在 LOOP 内 MERGE 链的 CSE 正确性（复现 = osr.md 本节三态矩阵 + tv_copy 诊断法：`__builtin_return_address` 对照 chunk entry 定位调用点），或 OSR chunk 换独立编译配置（FOLDING off + 可用的 O1 变体）。
- **2026-11-02 重验落地（f97555a 后无混杂会话，阻塞证伪）**：三态矩阵的 O1+FOLDING 坏码**不复现**——与 kv 线同款"卡 deps/ir 上游"归因被后续修复证伪。白名单重加三 op（`osr_body_ok` INIT_MCALL/INIT_NEW/NEW_USER），转正 `tests/jit/test-osr-005.php` 三形态（c1 方法调用循环 / c2 new+prop+COPY / c3 三者组合）：**10 连跑全绿、osr ir_check=1、JIT=0 对拍一致、241/241、bench 全带内（kv/object 等零回退）**。**chunk 执行性实证**：阈值关断对照（`TPHP_JIT_OSR_THRESHOLD` 临时改 10 亿构建 vs 默认）——方法+对象探针 4349→3755ms（**13.6% 实益**），排除"只编译不进入"假绿。**勘误**：阈值 env 此前从未接线（jit.h 注释与本文件曾称 `TPHP_JIT_OSR`/`TPHP_JIT_OSR_THRESHOLD` 可覆盖——纯文档漂移），改阈值需动 jit.h 宏；接线 env 记为可选后续。

### 批 2-4：INIT_MCALL 折叠直调（2026-11-02，P0-9 归因后的下一刀）

- **动机**：批2-3 只放行白名单，mcall 仍整序 opseq replay 经 C helper（pure-call 65 = empty 6.7 + replay ~58 vs PHP 39）——P0-9 归因定位的默认配置剩余大头。
- **机制（jit.c，`p_mcall` 态）**：INIT_MCALL 时 op->ic 已由解释器固化精确单态 user 方法（fill 规则 `decl==o->cls && fn && vis 已验`，非 native 非 magic）→ **三道运行时守卫**（接收者 T_OBJ / GCHead.kind==GC_KIND_OBJ / `TObj.cls == sm->decl`，读 recv 快照）全过 → 走 lower_call_finish 直调（poff=1，$this@slot0 = 快照 raw copy + addref，**调用后统一 release 配平**：callee move 走（`return $this` → slot0 置 UNDEF）则空操作、所有权随 ret 转移；entry/INT 合同早退均在 addref 之前无泄漏）；任一失配或形态不支持（ic 空 / entry 未备 / **callee 体含 replay op**（`jit_body_frameless` 扫描：prop4/ISSET_DIM/FE/INIT_NEW/NEW_USER/INIT_MCALL——直调链不推 VMFrame，真 handler 会读进调用方帧）/ 函数级）→ **原 p_seq=2 序回放**（语义完整兜底）。守卫失配 RETURN OSR_SEQ_PC = 从 INIT_MCALL 幂等重跑（OSR 位置安全；**函数级不折叠**，避开 deopt 重启语义）。INT 合同失配（字符串实参）同走 seq 重跑。
- **实测**：probe **pure-call 65→12.3-12.5ms（5.2×）**，vs PHP 39.5 = **0.32×（3.2× 反超，原 1.65× 落后）**；new+noop 121-122→66.5-78（vs PHP 93.5 转反超）；free/nop/empty/JIT=0 全不动。test-osr-005 扩 c4（`return $this` 引用平衡 + 字符串实参合同回退）、c5（多态 cls 守卫失配→解释器重跑 39999），5 场景 JIT=0/1 一致、osr ir_check 全 1、**241/241**、bench 两轮 min 回带（机器噪声期，spread 27% 系统性）。
- **过程发现（既有限制，非本批）**：参数位嵌套调用（`strlen($o->m())`——INIT_FCALL 先于实参求值）撞 p_callee 态拒编 chunk（backlog P0-16 记账同款）；c4 改独立语句绕开。

### 批 2-5：无 ctor INIT_NEW 折叠直落（2026-11-02 续，P0-9 下一刀）

- **动机**：probe new-only 60.7 / new+noop 121（vs PHP 49/85 落后）——mcall 折叠后的对象链 replay 残余。
- **判定点在 NEW_USER（首版踩坑）**：类 ic 快照挂在 **NEW_USER op**（`op_new_user` 自治解析填充，"emit 侧不读写"是旧注释）——首版在 INIT_NEW 读 `op->ic` 恒 NULL，fold 静默不生效（probe 无改善暴露），改判定点到 NEW_USER 后生效。SEND 两侧语义一致（p_seq 跳过，实参仅求值副作用、值留原槽由帧常规释放——无 ctor 不消费实参）。
- **fold 条件（emit 期全静态）**：`j->osr` + `p_seq==1` + ic 已固化（≠NULL≠1 哨兵）+ `m_ctor<0` + 非 `CLS_ABSTRACT` + `≠g_closure_cls`（vm.c 该符号转 extern 供 belt）+ 全部属性 `def_thunk==0`。**零 raise 面论证**：alloc（`tphp_obj_alloc`）= 池/malloc（OOM=abort）+ gc_register（扫描期 dtor 按就地 fatal 语义不外传）+ 属性默认值（无 thunk 即纯字面量 addref）；abstract/Closure 等 raise 型 site 由 threshold 前提排除（ic 固化前的 raise 会 unwind 掉循环函数够不到 back-edge 阈值——跨调用 counter 累积的极端形态由 emit belt 双拦）→ **零守卫零 setjmp**。
- **`lower_new_finish`（镜像 new_user_at 无构造器路径，净额等价）**：`tphp_obj_alloc`（**先于** dst release——GC 扫描时旧 dst 仍在槽内可达，同解释器顺序）→ dst：release 旧值 + 16B 存 TV_OBJ（alloc rc=1 接管，等价解释器 rc++/--/transfer）；无 dst：scratch 存放后 release 归零（同解释器净额）。清 p_seq（与 lower_replay 同位）。
- **守卫补口**：p_new 态进了 INIT_FCALL/INIT_DYN/INIT_MCALL/INIT_NEW/INIT_SMCALL/SEND 的忙闲检查（嵌套调用 → 拒编回解释器，与 p_seq 嵌套限制同款；SMCALL 在 new 序内无回放可依直接拒）。
- **实测**：probe **new-only 60.7→26.3-26.7（2.3×，vs PHP 49.4 = 反超 1.9×）**、**new+noop 121→30.1-30.5（4×，vs PHP 85-93 = 反超 ~2.8×）**；pure-call/free/empty 不动。test-osr-005 扩 c6（无 ctor new + prop + 累加 449985000），6 场景 JIT=0/1 一致、osr ir_check 全 1、**241/241**。**bench 无回退定案**：object 曾读 +3.5%（32.7 紧 spread）→ **stash 夹替轮 A/B 反转**（BASE 31.8/med33.9 vs NEW 30.9/med31.2）+ 机制排除（bench 全函数级编译**零 OSR chunk**，nfold 需 j->osr 对 bench 惰性）= 环境负载期假信号；array-push min 回带 54.3。
- **残余**：user-ctor 折叠（dst=对象而非 ret + ctor rc 转移 + native ctor 分支 + ctor RETURN 语义对齐，工程更大）；`new C($args)` 实参 dtor 时序微移（值留原槽至帧末释放 vs 解释器 NEW 时释放——同语句内、无 ctor 类，可接受已记账）。

### 批 2-6：linux CI test-osr-005 段错误——deps/ir LSRA 实参 hint 窗口 bug，chunk 内 COPY 改内联绕行（2026-11-02）

- **CI 现象**：linux x86_64 + mac x86_64 的 test-osr-005 挂在 c2（`$s2 += $u` TypeError "Unsupported operand types"），Windows 本地全绿——批2-3 放行的 new+prop+COPY chunk 形态首次在 SysV 上跑。
- **本地复现环境（重大基建）**：`wsl --install -d Ubuntu-24.04`（与 CI 同版）+ `apt clang binutils` + `sh build.sh`（注意：**rsync 带 CRLF 的 build.sh 要先 `sed -i 's/\r$//'`**；`/mnt/c` 可读源码；Docker Desktop 路线两次自爆后弃用）。复现：JIT=1 挂、**JIT=0 全绿** ⇒ 纯 JIT 生成码问题。
- **取证链**：chunk 机器码 dump（jit.c 临时 `TPHP_JIT_CODEDUMP`，已撤）→ objdump：`tv_copy` 调用点只装了 `rdi(dst)`，**rsi(src) 全程未装**；gdb 断 `tphp_jit_binop_slow`：`tb=876466986`（垃圾 tag）；gdb 断 `tphp_jit_tv_copy`：src=栈垃圾而真值 &t（slot5）正确躺在栈 spill；**ARGDBG**（dasc pass1 插针，已撤）实锤：`j=4 type=ADDR src=6(RSI) dst=6(RSI)` → emit 认定"值已在 RSI"跳过回装与拷贝。**Win64 对照反汇编**：同 IR 下 hint(RDX) 竞争败给 preserved r14，发出 `mov rdx,r14` 正确——**Win 绿纯属侥幸**。
- **根因（上游域）**：deps/ir **LSRA 把跨前置 call 活跃的实参值按 SysV hint 钉进 call 易失的 RSI 且不发回装**（ir_ra.c/ir_emit.c 与上游 HEAD b8d619a 逐字一致 = 上游同存，无现成修复可同步；cf39e5d/3470e7e 本地补丁与7个 post-vendor 上游提交逐一对拍排除）。**去 hint 试验（hints_count=0）被否决**：hint 是 call load pos 固定窗的承重结构，全局段错误——不能作为绕行。
- **绕行（落地）**：**OSR chunk 内 OP_COPY/OP_ASSIGN 改内联**（新 `lower_tv_copy_inline`：镜像 `tphp_jit_tv_copy` 的 addref→release→16B store，块结构同 dimic_take）——chunk 内不再产生"双 slot-addr 实参跨 call"的调用形态；函数级保持 helper 调用（历史全绿，不动）。
- **验证**：Linux 本地 test-osr-005 **3 连跑 c1-c6 全绿** + 全量 EXPECT sweep **219 PASS / 5 FAIL（5 个全是当时 CI SKIP 的 windows-only FFI（已移除），本地 runner 未实现 SKIP 而已）**；Windows **241/241**；bench 定点复测包内（机制上 bench 零 OSR chunk、函数级 COPY 路径未动）。
- **上游 issue 不提交（用户拍板 2026-11-02）**：dstogov/ir 作者疑似停更——**绕行转正为长期方案**（非临时），deps/ir 转为**本仓自维护**（后续上游域 bug 自行修复，vendor 时照旧对拍上游提交）。同族风险核查：CONCAT 等跨 call 实参形态**不在 osr_body_ok 白名单**（chunk 内只有 COPY，已内联）；函数级 helper 路径历史全绿。issue 草稿作废（%TEMP%\dstogov_ir_issue.md，诊断结论已全量入档本节 + memory 续7）。

### f0：无帧直调 × replay 体——opseq 自框化 temp VMFrame（2026-11-02 立，11-03 落地）

- **现象（HEAD 既有 bug）**：JIT→JIT 直调是裸 `entry(vm, slots, ret)`（tphp_jit_call / call_slots 不推 VMFrame），而 `tphp_jit_opseq` 以 `vm->x.frame` 为回放帧——callee 体含 replay op（new/prop/foreach pad）时，无帧入口拿**祖先帧**按 callee pc 索引 ops → 读写错误帧槽 → 乱执行/TypeError（test-frameless-replay-001：递归 fr_rec 第二层 `$q->v` 读回 unknown）。批2-4 只在 INIT_MCALL 折叠门挡了 `jit_body_frameless`；INIT_FCALL 静态直调 / p_dyn 链 / call_slots 无守卫。
- **修复（重新实现，见批2-7 节事故）**：`tphp_jit_opseq` 加 `fn/slots` 参数（lower_replay 传 `j->fn` 编译期常量 + `j->r_slots`）；入口 `orig->fn != fn || orig->slots != slots` 即**栈上自框化** temp VMFrame（memset 0 + fn/file/pc/slots/prev=orig，`vm->x.frame=&tmp`）；escaped 与正常两个出口 `vm->x.frame = orig` 撤框。解释器/OSR 入口（帧已匹配）零成本跳过。temp 非池分配：unwind 的 floor 检查先于 frame_destroy → 永不被弹/被释放；**递归同 fn 不同 slots**（scratch）靠 slots 比对识别。
- **验证**：test-frameless-replay-001 五场景（静态直调 / 递归 / 方法直调 / 逃逸 raise / 纯标量对照）JIT=0/1 一致；最小复现（递归 × new+prop）修复；全量 **242/242**。

### 批 2-7：函数级折叠直调（2026-11-03，INIT_MCALL/NEW_USER 扩函数级 + mfail 回放）

- **动机**：批2-4/2-5 折叠均限 `j->osr`（OSR chunk），函数级仍整序 p_seq replay——probe fn-mcall 262.9 / fn-new 395.3 落后，函数级热点（非循环体调用）吃不到折叠收益。
- **核心难点 = 函数级 deopt 重启语义**：OSR 守卫失配可 RETURN OSR_SEQ_PC 从 INIT_MCALL 幂等重跑（实参表达式未求值，无双执行）；**函数级 RETURN 0 = op_do_fcall 从 pc 0 重启整函数 → 序前副作用双执行**。
- **Route A（落地）**：函数级守卫失配不 RETURN，改收集 `j->mfail[]` → OP_DO_FCALL 处 `esucc=ir_END()` → `ir_MERGE_N(mfail)` → `lower_replay(p_seq_pc, pc+1)` **只重跑调用序**（实参已由 IR 无条件求值，opseq 跳过表达式 op 恰正确；recv 快照 r_call_clo = INIT 时接收者，PHP 本就只求值接收者一次）→ ereplay 与 esucc `ir_MERGE_2` 汇合（镜像 p_dyn slow[] 模式）。**ok==0（callee deopt/异常）保持 RETURN**——callee 已部分执行，mfail 回放会双执行 callee；异常链须直达 op_do_fcall longjmp。arg_copy 是 raw 拷贝无 addref → mfail 弃 scratch 零泄漏；$this addref 在契约检查之后，mfail 早退全在 addref 之前。
- **落点**：INIT_MCALL fold 条件删 `j->osr &&`（函数级只设 p_callee/p_mcall/p_mcall_cls/p_argc，不发 OSR 守卫 RETURN）；三守卫 / entry==NULL / INT 契约在 `mf = p_mcall && !osr` 下改记 mfail；OP_NEW_USER nfold 删 `j->osr &&`（全静态检查零 raise 面）+ fold dbg 打印；**`op->a2 == n_params` 静态门**（省略可选参 → 缺参槽是 UNDEF 不是默认值、多传实参 → 覆写 callee 局部槽 → 直调布局不成立；不折叠走 p_seq=2 回放 RECV 补默认值 = 批2-7 前行为，否则 lower_call_finish L1873 `argc != n_params` 会 `j->fail` 拒编整函数）；`jit_compile_callees` 加 INIT_MCALL 分支（ic=TMethod* 取 sm->fn，ic 空/无 fn 跳过）；`tphp_jit_defer_ic` 挡 op_do_fcall 与 tphp_vm_call_callable 两处 eager 门（含 ic 未固化的 INIT_MCALL/NEW_USER——ic 由解释器执行该 op 填充、首调必 NULL；**只挡 eager，hot≥127 强制编译不延退**；NEW_USER ic==1 解析失败哨兵不延退）。
- **mfail 上限**：单 site ≤ 3 守卫 + entry + 9 契约 = 13 < `mfail[16]`。
- **实测**：probe min-of-5 **fn-mcall 262.9→193.9（1.36×）、fn-new 395.3→314.3（1.26×）**、fn-ctor 403.8-436（基线 418 噪声带）；DEBUG 实锤 `fold mcall id pc=2 [fn]` / `fold new PrPlain pc=2 [fn]`、ir_check=1、check=2000000/1999999000000/1999999000000 JIT=0/1 一致；mfail 三路径 ad-hoc（守卫3 多态 cls / 守卫1 标量接收者 / INT 契约 n=3 证明无重启双执行）JIT=0/1 一致；可选参场景输出正确无 FAILED emit；**全量 242/242**；bench-rounds 5 轮全项带内（fib 10.6 / call 6.2 / push 36.8 / read 7.3 / kv 29.5 / object 24.7 / logstat 47.5，kv/object 相对首版 +8~9% 落在 spread 22%/11% 噪声带内）。
- **过程事故（源文件神秘还原）**：13 处编辑在 0:59:27 被同秒还原（恰为传给失败 `git stash push -- src/jit.c src/jit.h src/vm.c` 的三个路径；非仓库 stash 不可能成功，机制未明，疑 IDE/沙箱）→ 按会话上下文全部重放 + 备份 `%TEMP%\b27bak\`。**重放漏了 f0 的 opseq 自框化** → 全量 241/1（test-frameless-replay-001 fr_rec TypeError）→ 11-03 重新实现（上节 f0）。教训：编辑后、测试前校验源文件内容；本环境避免 git 命令。

### 批 2-8：user-ctor 折叠直调（2026-11-03，NEW_USER cfold 门 + lower_new_ctor_finish + emit pass 1.5）

- **动机**：object bench 下一刀——批2-5 只折 `m_ctor<0`，带 userland ctor 的 `new` 全走整序回放；probe fn-ctor 是该形态纯量。
- **实现**：NEW_USER 门三分 nbase/nfold/**cfold**（fn 存在/非 abstract/非 native/`cfn==j->fn||jit_entry`/`!jit_ctor_ret_val` 有值 return 不折/`p_argc==n_params`）；`lower_new_ctor_finish` = **守卫前置**（entry belt + 逐参 INT 契约全过才 alloc，失败路零垃圾）→ slot0 raw+addref → `entry(vm, slots_sc, ret_sc)` 直调 → raise 路 `release(slot0)+tphp_jit_ctor_half_cleanup(DTORED 半成品不跑 dtor)+RETURN` / 成功路 `release slot0(2→1)+release ret_sc+dst raw store 不 addref`（净额逐位对照解释器 new_user_at + ctor-RETURN 的 P0-2 语义）；cfold 后 mfail 聚合复用批2-7 MERGE_N 形状；`jit_compile_callees` +NEW_USER 分支（连带编 ctor，emit 期 entry 就绪）。
- **⚠ 崩溃根因（deps/ir，cf39e5d 的反例）**：ctor 首折即崩（opseq 收到 fn==slots）——post-RA 实锤 CALL 读点 slots 钉 %rdx、fn 常量装载目标亦 %rdx，`ir_emit_arguments` "装载先于洗牌"顺序令 `movabs fn→rdx` 先冲掉待搬的 slots。cf39e5d 的"LSRA 每 arg 固定窗"论证对 CONST 实参不成立（窗口与读点同一 IR_LOAD_LIVE_POS tick、半开区间无冲突判定）；MEM 装载（恢复 RA 段，concat 场景）与 CONST 装载（新值，须后置拷贝）**同形状反方向**，emit 无法自辨。**修复 = pass 1.5**（四文件同调 ir_x86.dasc/ir_emit_x86.h/ir_aarch64.dasc/ir_emit_aarch64.h）：pass1 记 `const_dsts`，洗牌前先发射"源=常量装载目标"的拷贝子集（CONST dst 恒非拷贝目标 → 子集独立）；MEM 装载保持原位（cf39e5d 不回退）。只有 ctor 形态命中：其 slots 区间 [entry..call] 短钉 rdx home，普通函数 slots spill/跨 call 不在 rdx。
- **测试6 前提勘误（原设计双重失效）**：原"~main try-loop OSR fold raise"不成立——~main `n_ex>0` 在 silent 门前拒 OSR（无 reject 日志）、`new`-invoked ctor 无解释器热门（编译只靠 enclosing jit_compile_callees）。重构为：`t3($b)` 单次 10020 长循环（**OSR 阈值 10000 backedge > 调用阈值 127——短循环多调用永远先函数级编译够不到 OSR**）+ 永不执行 new 站点拖住 defer_ic 急切 + `tw3` 暖机 ctor entry（OSR 编译不走 jit_compile_callees）→ **`fold new-ctor NC3 [osr]` 与 `[fn]` 双触发**，raise 走 ok==0 半成品路。
- **实测**：**243/243**、JIT=0/1 逐字 identical；probe 背靠背 A/B min-of-5：fn-ctor **556.0→475.2（-14.5%）**，fn-mcall/fn-new 持平（emit 零回归）；bench object A/B 持平带内（BASE min30.9 vs FIX min30.8，当轮噪声期 spread 12-17%）；当轮 harness 绝对值普涨 20-60% = 环境噪声，跨轮不可比。详 memory/2026-11-03.md 批2-8 收口节。

### 3.1 触发：back-edge 计数（解释器侧）

- 判定点：`op_jmp` 中 `target <= pc`（向后跳 = back-edge；JMPZ/JMPNZ 向后跳在 JIT 侧是 trap_fail 形态，不做计数点）；
- 计数器：**`TOp.ic` 槽复用**（JMP 类 op 无 DimIC 占用，无冲突），**per-site** 三态编码（落地版，替代原"每函数最热 1 循环"设计——见 §3.2）：
  `0..THRESHOLD-1` = 计数器；bit63 tag = 该位点 OSR chunk 入口（用户态代码指针 bit63 恒 0）；`UINTPTR_MAX` = 熔断；
- **阈值**：`TPHP_JIT_OSR_THRESHOLD`，默认 **10000**（back-edge 次）——保守，理由：OSR 编译一次 ~10-85ms 塞进循环中间，阈值过低对短命循环纯亏（`n_ops × 编译成本/迭代收益` 盈亏平衡约 1e4 量级迭代）；函数级阈值 127 不适用（back-edge 每迭代一次）；
- 成本：每 back-edge `load ++ cmp store` ≈ 1-2ns，占 dispatch 基线（67ns/轮）<3%；
- **不递归计数**：内层循环 back-edge 只计内层；嵌套循环外层循环体内含内层 back-edge 跳转不误触（JMP 目标判定天然按 site 归属）。

### 3.2 编译单元：单自然循环（v1 范围）

- 循环识别（编译期，静态）：header = back-edge 目标 pc；循环体 = header → back-edge 的可达 op 区间（v1 用**线性区间近似**：`[header, back_edge_pc]` 内、且跳转目标不落在区间外的 op——多出口循环的额外出口视为 exit）；
- **per-site 独立编译**（落地版，替代原"每函数最热 1 循环"）：每个 back-edge site 的 `TOp.ic` 自行计数/存自己的 chunk 入口（bit63 tag）/自行熔断。原设计（`TFunc` 增 `osr_entry/osr_header/osr_exit` 字段、每函数只编最热 1 循环）**已废弃**——实测踩坑：函数级入口被同函数第二个热循环复用，A 循环的 chunk 读 B 循环的槽，cond 常假立即返回 A 的出口 pc，A/B 间 ping-pong（4000 万圈，值全对但 B 永远到不了）；per-site 后每个循环独立计数独立编译，`TFunc` 零新增字段；
- 白名单：循环体 op 集 = ir-jit §6 白名单 + P0 系列已落地扩展（IC 内联链 / prop 回放 / CONCAT helper 等，与函数级 eligibility **共用同一判定函数**，传 pc 区间参数）。批 1 白名单见 §7 批 1 落地记录。

### 3.3 OSR entry ABI

```c
/* OSR 版循环体；slots = 解释器当前帧（零拷贝，直读直写）。
 * 返回值 = resume pc（解释器应从该 op 继续）：正常 exit = 出口 op 的 pc；
 * deopt/fail 点 = 失败 op 自己的 pc（解释器重跑该 op，批 1 白名单幂等）。 */
typedef int (*OsrEntry)(VM *vm, TVal *slots);
```

- IR 结构：`START → PARAM(vm=1/slots=2) → emit_seq(header, stops=exits)`——与函数级唯一区别：**入口 pc = header**、prologue 不做任何槽初始化（解释器帧现成）；所有 fail/deopt 点 `ir_RETURN(ir_CONST_I32(cur_pc))`（OSR_RET_PC 宏，函数级同点位返 0）；
- **终端 END 收尾（落地时踩坑）**：`emit_seq` 命中 stop 返回 `TR_END(end, target)`——TR_END 仅 OSR 出现（函数级 stops=NULL 永远 TR_RET/FAIL/LOOPBACK），出口块必须 `ir_BEGIN(t.end) + ir_RETURN(t.target)` 终止，否则 codegen 落出代码缓冲（实测：垃圾 resume_pc → VM 跳飞挂死；ir_check 不查尾块 terminator）；
- 循环回边：现有 `trap_lb` 机制原样复用；
- **JIT→JIT 调用**（循环体内 do_fcall 直调 callee）：direct-slot 机制原样复用（callee slots 为 ALLOCA，与帧 slots 无关）——调用语义零新增；
- code 管理：**per-site 挂 back-edge op 的 `TOp.ic`**（bit63 tag，一次写全局只读；线程模型同 §10 ir-jit）。

### 3.4 exit 与 deopt（复用 P0-12）

- **正常 exit**：循环条件不成立走主 exit path → `*resume_pc = osr_exit` → 解释器从循环后继续（循环体内已写的 slots 就是解释器状态——同布局免恢复）；
- **deopt**：体内 helper fail / IC 链 miss 回退 / raise 不可能（封闭世界）——fail 点的**解释器 pc = 编译期已知**（emit 时记 op→pc 映射，helper 返回路径携带）→ 解释器从该 pc 重跑该 op（op 幂等性由现有 deopt 语义保证，P0-12 已验证同款）；
- **协程/异常**：封闭世界无挂起点、无 raise——与函数级 JIT 同结论，无新增面。

### 3.5 编译停顿治理

OSR 编译同步塞在循环中间（解释器循环跑到 1 万次时停 10-85ms）。v1 治理：

1. **保守阈值 10000**（§3.1）；
2. `n_ops ≤ 96`（急切编译同款上限，超限不编——大循环体编译成本失控风险）；
3. **P2-13 异步编译**（后台线程，ir-jit §12.3 / jsc-lessons §5）作为批 3 联动项——v1 同步编译 + 保守阈值兜底；
4. 编译失败熔断：`TOp.ic = UINTPTR_MAX` 永不再试（同函数级 `hot=UINT16_MAX` 语义）。

### 3.6 与函数级 JIT 的协同

函数已 JIT'd（jit_entry 有值）→ 解释器不执行其循环 → back-edge 计数不增长（触发点在解释器 handler 内，天然互斥）；OSR 版函数不设函数级 jit_entry（两者独立、可共存）。

## 4. 实施批次

| 批 | 内容 | 验收 |
|---|---|---|
| 批 1 ✅ | back-edge 计数基建 + 单循环 OSR（int 域封闭子集，白名单见 §7） | ✅ 2026-10-31：`tests/jit/test-osr-001.php` 6 用例（for/while/break+continue/INC/嵌套/while+if）TPHP_JIT=0/1 对拍全对；234/234；20M 迭代 int 循环 **2.6-2.8×**（2140→803ms） |
| 批 2-1 ✅ | 循环体白名单加 **FETCH_DIM_R**（读操作幂等，deopt 重跑安全；形态守卫对齐函数级 eligibility） | ✅ 2026-10-31：`test-osr-002.php`（kv 双循环嵌套 dim/packed 直取/tmp 键）+ `test-osr-003.php`（undefined key/non-array/packed overrun 的 raise→deopt→解释器重跑边界，n_ex>0 拒编路径）对拍全对；**236/236**；顶层 kv 形态 195→**58ms（3.4×）**；归因修正见 §7 |
| 批 2 | 循环体白名单扩到 IC 内联链（FETCH_DIM_R）/prop 回放/do_fcall 直调 | bench-vs-php **kv-read / array-read / real-logstat 进 JIT 形态**（今天全是解释器）；敏感簇数字 |
| 批 3 | 多循环表 + P2-13 异步编译联动 + 阈值自调 | 编译停顿 <1ms 体感；M3 组合收官（多循环已随 per-site 落地，批 3 余异步编译+阈值自调） |

## 5. 测试与验收（M3 判据）

- `tests/jit/test-osr-*.php`：①正确性——单层/嵌套循环、循环内 break/continue、循环变量 int 溢出提升、循环体含调用、TPHP_JIT=0/1 对拍；②触发——计数精确性（差一次不触发）；③熔断——失败循环不反复编译；④deopt——循环中 deopt 后解释器续跑结果正确（含 deopt 后循环退出继续函数尾）；
- 性能：`tools/perf-compare.ps1` 对比表——批 2 目标 kv-read / array-read 相对今天（恒解释器形态）显著下降；M3 = 敏感簇 ≤1.0×（与 P0-13/P0-14 合力）。

## 6. 风险与不做清单（防翻案）

| 项 | 决策 |
|---|---|
| 多态/投机类型（JSC DFG 式 speculation） | **不做**——tphp 是 baseline 档，白名单静态封闭；类型投机属 P0-17③ 类型特化，另立项 |
| OSR exit 的反向（JIT→解释器逐 op 精确恢复中间状态） | v1 只需 resume_pc + slots 同布局；不做 DFGOSRExit 921 行级逐 slot 物化（tphp 无需） |
| 循环体含 raise/generator/（历史）FFI | 不做——封闭世界白名单排除，同 ir-jit §13 |
| 编译停顿 | v1 同步 + 阈值 10000 + n_ops≤96；异步化归 P2-13 批 3 |
| on-stack 值替换/逃逸分析 | P0-9/P0-17② 域，不混入 |

## 7. 批 1 落地记录（2026-10-31）

- **白名单**（`osr_body_ok`，比规格 §4 批 1 行更宽但仍封闭）：CONST/COPY/ASSIGN/ADD/SUB/MUL/NEG/POS/INC/DEC/BW_*/SHL/SHR/BOOL_*/CAST/EQ/NE/SEQ/SNE/LT/LE/CMP/ISSET_CV/UNSET_CV/JMP/JMPZ/JMPNZ；CALL/DIM/ARRAY/RETURN/FE/EXTRACT 一律拒；
- **拒编形态实测**：循环体内 `break` → loop-cond 处理器"body must back-edge or fail"→ 熔断回解释器（test-osr-001 case 3 走此路，值正确）；`while(1)` 无条件回边 → TR_LOOPBACK 顶层冒泡拒编；
- **两个落地 bug**（均为首次运行暴露、已修）：
  1. 终端 END 块无 terminator（§3.3）→ 垃圾 resume_pc → VM 跳飞挂死；
  2. 函数级 osr_entry 复用 → 跨循环 ping-pong（§3.2）→ 改 per-site `TOp.ic` bit63 tag 编码；
- **门槛**：`is_main` 允许（OSR 不走函数入口 ABI，顶层循环正是核心场景）；仍拒 is_global 别名槽/variadic/generator/异常表/n_cap>0/typed params；
- **触发链**：`op_jmp`（`ar <= cur`）→ `tphp_osr_backedge`（计数→编译→bit63 tag 直进）→ `tphp_jit_compile_osr` 返回 entry（NULL 即熔断）。

### 批 2-1 落地记录（2026-10-31，FETCH_DIM_R）

- **改动最小化**：仅 `osr_body_ok` 加 `OP_FETCH_DIM_R`（+ t2≠OPD_UNUSED 形态守卫对齐函数级 eligibility）——emit 复用 emit_seq 的函数级 case（packed 直取 + DimIC 链全内联），零新 emit 代码；
- **语义安全论证**：读操作幂等（deopt 重跑安全）；helper raise_guarded 不穿 IR（P0-12），异常由解释器重跑该 op 时抛出，无重复副作用；IC 懒分配 emit 期单次（触发线程即执行线程，同步编译无并发执行）；dimic_take 先 addref 后 release，重跑时 dst 旧值同源，引用计数自平衡；
- **n_ex 门槛注意**：main/函数里**任何 try/catch → n_ex>0 → 整函数拒编**（既有保守门）——test-osr-003 即走此路（拒编安全回退 + 异常语义对拍）。批 2 后续如要"循环体内 try + 循环体白名单内"形态，需 per-loop 异常区间检查，暂不立项；
- **归因修正（批 C 定性更正，不影响当时决策）**：bench 闭包形态（kv-read/array-read）**在批 1 前已被函数级急切编译**（闭包 n_ops≤96），循环在函数级 JIT 里执行；批 C 的 avg_scan=4.48 插桩来自 **IC 链 miss → v1 helper 内部的 tarr_get_ic**，非纯解释器执行。因此 harness 的 kv-read 4.2×（195→46）非 OSR 增量（批 C 时已 ~47ms）；**OSR 批 2-1 的真实增量场景 = main 顶层循环 / n_ops>96 大函数内的循环**（函数级够不着的形态）：顶层 kv 形态 195→**58ms（3.4×）**，DEBUG 实锤 0 次函数级 compile、纯 OSR 贡献；
- **real-logstat harness 顺序效应（与 OSR 无关，记录）**：完整 harness JIT=1 下 real-logstat 82-95ms vs JIT=0 43-54ms；但单跑该形态 64 vs 66ms 零回退，且 harness 全程 0 次 osr compile——归因前序项函数级 JIT 化后的内存/缓存状态连带 + 本机噪声（±10-20%）。验收以单项形态为准，CI（perf.yml）机器态干净待复核。

### 批 2-2 落地记录（2026-10-31，折叠 CALL 序 + prop ops）

- **语义前提（关键论证）**：`tphp_jit_opseq` 返 0 **恒等于 exception**（vm.c setjmp 唯一出口，prop handler/builtin 错误全走 raise）→ 异常传播不重跑，prop 单 op pad（lower_replay）的 deopt resume=cur_pc 恒安全；
- **折叠 CALL 的 resume 语义**：直调 guard fail（entry NULL / INT 契约 / callee deopt 未执行 / call_dyn ok=0）发生时 **INIT..SEND..DO 序未完整执行**——单独重跑 DO 语义断裂（args 栈/rec 缺 INIT）→ 新宏 `OSR_SEQ_PC(j)`（osr ? p_seq_pc : 0）：**从序起点重跑整序幂等**（SEND_VAL/SEND_VAR 源恒为槽；args 表达式在 seq 之外求值，不在重跑范围）；函数级=0 行为不变。**INIT_FCALL/INIT_DYN emit case 补设 `j->p_seq_pc`**（此前只有 INIT_NEW/INIT_MCALL 设——函数级不需要，OSR 需要）；
- **坑 1（c4 段错误实锤）**：OSR 编译路径漏分配 `j.r_call_slots`——B 直调链读它得 ref 0 → **ir_check=0 照常出码 → 崩**（P0-13"ir_check 不 gate"铁律再犯；O0 下 ir_jit_compile 返 NULL 熔断反而安全）。修复 = OSR 路径补 ALLOCA（函数级 2384 行同款）；
- **坑 2（测试构造）**：main 含任何 try/catch → n_ex>0 → main 全部循环拒 OSR（批 1 静态守卫）——异常传播 case 用"循环函数无 try + 顶层壳函数 catch"构造；osr_body_ok 补拒因日志（`[jit] osr reject: pc=N op=X`，对齐 eligibility 审计模式）；
- **白名单**：osr_body_ok +9 op——prop 四 op（FETCH/ASSIGN/ISSET/UNSET_PROP）+ INIT_FCALL（用户 callee 递归准入/builtin ic|1）/SEND_VAL/SEND_VAR（const 源限标量串）/DO_FCALL/INIT_DYN；
- **实测**：test-osr-004.php 6 场景三方一致（PHP 8.5 / JIT=0 / JIT=1）——c1 用户直调、c2 builtin、c3 prop 读写、c4 动态调用（捕获闭包→call_dyn trampoline）、c5 callee mid-loop raise（raises=1 证明无 deopt 重放）、c6 表达式实参；5 个 OSR chunk 全编译（header=2/23/47/78/124，ir_check 全 1）；**238/238**、bench all-ok；
- **性能**：OSR×CALL 基准（main 顶层 20M 次调用 b1($i&255)，OSR 独占场景）3720→**1650ms（2.25×）**，PHP 8.5 JIT 1010ms（0.61×）；
- **遗留（批 2-3 候选）**：INIT_MCALL/NEW_USER 方法与对象链循环形态（opseq replay 同构，白名单一行）；UNSET_CV 已在批 1 白名单；FE_RESET/FE_FETCH 循环形态。
