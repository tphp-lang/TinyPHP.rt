# PLAN/jsc-lessons.md · JSC JIT 借鉴与准入策略升级规格

> 状态：2026-10-28 立项（用户拍板："先记录、有帮助的先搬过来、以后慢慢完成——v0.0.2 不用担心"）。
> 分析对象 = 本机 `C:\Users\kllxs\Desktop\JavaScriptCore-7611.3.10.1.3`（完整 WebKit/Safari 17.x JSC 源码树，jit/ 120 文件）。
> 定位：**模式级借鉴，代码不可搬**——值布局（NaN-boxing/Butterfly）、并发 GC、C++/asm 工具链全不同；搬的是分层策略与四机制设计。任务账：backlog P0-13 / P0-14 / P1-12 / P2-13（防双账，细节只在本文件）。
> 上级索引：[../PLAN.md](../PLAN.md)。性能战略：[perf-roadmap](perf-roadmap.md)。JIT 规格：[ir-jit](ir-jit.md)。

## 0. 一句话结论

tphp 缺的不是更强的 JIT 后端（ir 后端已被 spike 证实不弱：手写 fib IR 1.2ms vs Zend tracing JIT 10.9ms），而是 JSC 的 **"Baseline 档"**——在解释器与完整 JIT 之间，用 **inline cache + profiling** 把白名单外形态（kv-read 5.84×、array-read 4.22×、object ~3×、call 16×）先提起来。

## 1. JSC 四层栈 vs tphp 两层对照

| 层 | JSC | tphp 现状 | 差距 |
|---|---|---|---|
| 解释器 | LLInt：asm threaded dispatch（`jmp [opcodeMap+opcode]`，`llint/LowLevelInterpreter.asm:351` dispatch 宏），VM 状态全寄存器化 | C VM；L4 全宏化在 P2-16 未做 | dispatch 基线 67ns/轮 vs PHP 16（P0-4 定性） |
| **Baseline 档** | 每字节码 site 内联 fast path + repatch 机器码 stub（IC）+ ArrayProfile/value profile 埋点 | **无此层** | 白名单外形态无任何快路，全落解释器 |
| 优化 JIT | DFG：类型投机 + OSR exit；Contiguous 数组读 = 1 次 bounds check + 1 次 load64 + hole check | ir baseline JIT v1.2：静态白名单准入 + deopt（P0-12 闭环） | 准入静态封闭；无循环 OSR |
| 重型 JIT | FTL：B3/Air 后端 + 异步 JITWorklist | 不对标（ir baseline 路线已定） | — |

## 2. 机制 A：per-site 单态 IC（→ backlog P0-13，性价比最高）

**JSC 证据链**：
- `jit/JITPropertyAccess.cpp:46` `emit_op_get_by_val`——Baseline 编译时内联生成 fast path（`JITGetByValGenerator`），慢路径 call `operationGetByValOptimize`；
- `jit/Repatch.cpp` + `bytecode/PolymorphicAccess.cpp` + `bytecode/AccessCase.cpp`——慢路径观察实参形态，repatch 把"该 site 本次形态"焊成机器码 stub；`bytecode/AccessCase.h:112` 案例族：`IndexedContiguousLoad / IndexedDoubleLoad / IndexedArrayStorageLoad / Transition / Getter / Miss …`，多态 ≤N case 链式分发，超限回 generic；
- DFG fast path 佐证（`dfg/DFGSpeculativeJIT64.cpp:2723` Contiguous in-bounds）：`branch32(AboveOrEqual, idx, [storage+publicLength])` → `load64 [storage+idx*8]` → hole check。**IC 命中路径的最小形态 = 1 次形状比较 + 1 次取值**。

**tphp 设计草案（解释器侧独立生效，不依赖 JIT）**：
1. TOp 附 per-site IC 槽；site 状态机：`未预热 → 单态 → 多态(退 generic) / 失效(退慢路径)`；
2. 单态缓存内容：容器形状（kind/packed/hash 标志）+ normalized key（intern 串指针或 int key）+ 桶索引快照；命中 = 形状比较 + key 同一性比较 → 直取，**跳过 norm/hash/全桶扫描**（kv-read 现在的 84.9ms 主要就是这些）；
3. 失效条件：数组形状变化（packed→hash、rehash、resize、cow 写）→ invalidate（与 L5 惰性桶同步语义兼容）；object 同理：add_prop/redeclare/traits 变更失效；
4. object 属性（FETCH_PROP/ASSIGN_PROP）IC：缓存 TProp 槽索引，命中直读写；
5. 与 JIT 关系：IC 在解释器生效即可收大头；后续 JIT 内可把单态 IC 焊进 IR（后置，不在 v1）。

落点：src/opcode.h（site 槽布局）、src/vm.c（op_fetch_dim_r / op_assign_dim / fetch_prop 族）、src/value.c、src/class.c。
验收：kv-read / array-read bench 进 2× 带；224/224 绿。
与 P0-3 关系：P0-3 字面量键 hash 缓存 = 本机制的静态特例雏形；P0-3 剩余项（opcode 级预存 hash）**并入本机制实现**，P0-3 收敛。

**v1 实测（2026-10-28，已落地）**：`DimIC{arr,bk,boff,val_off}` 24B 快照挂 `TOp.ic`（DIM 类 opcode 该字段本就空闲）；解释器 `op_fetch_dim_r` + JIT helper `tphp_jit_fetch_dim_r`（emit 传 `ir_CONST_ADDR(ic)`）双路接入；失效验证链 = arr 指针 + buckets 指针（覆盖 rehash/扩容/惰性重建）+ 桶 key 匹配 + val_off 非 tombstone——全部锚定当前数组真实状态，DimIC 字段非原子竞态写最坏 miss 不会错命中（Worker 线程安全推理见 value.h/array.c 注释）。**结果**：kv-read 58.4→50.3ms（-14%）、array-read 15.6→14.4ms（-8%）、全表无回退、224/224 绿。**未达"进 2× 带"**——IC 已削掉 norm/hash/探测，剩余 31ns/次 ≈ helper 调用框架 + JitEntry 开销（PHP 侧 6.3ns 是全内联机器码）。**v2 方向 = IC 命中路径内联进 IR**（emit 直发比较链 + load，miss 才 call helper——JSC Baseline IC 的完整机器码形态），属 ir-jit 白名单深化，独立小立项。

## 3. 机制 B：profile 驱动 JIT 准入（→ backlog P0-14）

**JSC 证据**：
- Baseline 埋点：`emitArrayProfilingSiteWithCell`（ArrayProfile：容器形态 seen bitmask）/ `emitValueProfilingSite`（值形态预测），均 per-site；
- DFG 消费：`dfg/DFGArrayMode.cpp` 据 profile 做 ArrayMode speculation，`speculationCheck(OutOfBounds / LoadFromHole …)` 失败走 OSR exit；
- **准入由运行期观察决定，而非编译期静态白名单**。

**tphp 设计草案**：
1. per-site 观察计数（复用机制 A 的 IC 槽状态，顺风车）：opcode 形态（操作数类型/形状组合）稳定 N 次即记"热且形态单一"；
2. eligibility 扩展：静态封闭类型世界（ir-jit 现行准入）之外，允许 **profile 证明形态稳定的 site 进 JIT**，deopt 兜底（P0-12 传播链已闭环，raise/deopt 基建就绪）；
3. 与 ir-jit §13 "v1 不做数组/字符串体内" 不冲突：准入面扩大后数组/字符串 op 仍走 helper + deopt，不新增 IR 内数组语义；
4. 防抖：形态翻转（int↔串↔数组）计数清零 + 翻转次数超限永久拒绝，防 deopt 风暴（JSC speculation-failure 计数同思想）。

落点：src/jit.c（eligibility + entry 采样）、src/compile.c（site 元数据）。
验收：白名单外形态 site 覆盖比例；13 项几何均值向 ≤3× 收敛。

## 4. 机制 C：循环 OSR（→ backlog P1-12，M3 钥匙，大工程规格先行）

**JSC 证据**：
- `dfg/DFGOSREntry.cpp`（416 行）：LLInt 循环计数触发，解释器栈帧 → JIT 帧映射（`DFGOSRAvailabilityAnalysisPhase` 求 live 状态）；
- `dfg/DFGOSRExit.cpp`（921 行）：投机失败 → 逐 slot 恢复解释器状态回退执行。

**tphp 动机**：函数级准入抓不住"循环体含白名单外 op"的函数（整函数拒绝）——敏感簇 call/read/kv 大量发生在循环内；A6 dispatch 67ns 也多在循环里。循环 OSR + 机制 A/B 是 M3 敏感簇反超的主力组合。

**立项前提（先出规格再动手）**：循环体 op 子集界定、VMFrame→JIT slots 映射、loop header OSR entry 检查成本上限、exit 路径与异常/协程交互（复用 P0-12 框架）。
关联：P0-9 第二层结构性反超、P0-11 dispatch 主线。

## 5. 机制 D：异步编译 = P2-13（小账）

**JSC 证据**：`jit/JITWorklist.cpp`（344 行）——任务队列 + 后台工作线程 + 完成回调 arm，当前帧继续解释不等编译。
**tphp 落点**：消除急切编译停顿（ir-jit §12.3 已立）。约束：编译线程不得触碰 VM 堆——候选形态=仅读 freeze 后只读的 Program（字节码跨线程只读共享已是现状），产物 JIT entry 原子挂回。

## 6. 不搬清单（防翻案）

- DFG/FTL 整体（`DFGSpeculativeJIT64.cpp` 14915 行 + B3 + Air）：工程量 = ir JIT 几十倍，依赖 JSC 值布局/并发 GC，负资产；
- NaN-boxing 值布局、Butterfly 对象布局、Concurrent GC（tphp = mimalloc + tval，已定）；
- wasm / yarr（tphp 已 vendor pcre2）；
- LLInt asm 解释器整体照搬（P2-16 全宏化已覆盖其核心收益 = computed-goto threaded dispatch；JSC 佐证该方向正确）。

## 7. 实施批次建议

| 批 | 内容 | 目标 |
|---|---|---|
| 批 1 | A 单态 IC（kv/array 先）+ B 观察计数基建（搭 IC 槽顺风车） | kv/array 进 2× 带；几何均值向 ≤3× 收敛（M2 收口路径） |
| 批 2 | A 扩展 object 属性 IC + 多态 case 链 | object 进 3× 内 |
| 批 3 | C 循环 OSR（规格先行，评审后开工） | M3 敏感簇反超主力 |
| 横切 | D 异步编译（P2-13） | 消除编译停顿 |

每批验收 = `tests/run.ps1` 224/224 全绿 + `tools/perf-compare.ps1` 对比表（禁改期望、真跑协议不变）。
