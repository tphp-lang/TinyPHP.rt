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
| 循环体含 raise/generator/FFI | 不做——封闭世界白名单排除，同 ir-jit §13 |
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
