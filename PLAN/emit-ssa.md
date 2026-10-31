# P0-17③ 发射器值流手术（emitter value-flow / SSA-ization）

状态：**设计稿 v1（2026-10-31）**——立项依据、方案、失效规则、分批计划。
预估：数天级。主攻目标 = Linux x64 CI 四重灾项（array-rmw 8.86× / object 6.38× /
kv-read 6.21× / array-read 6×）与 array-push 2.29×/object 的公共根因。

## 1. 问题定义（归因证据链）

每条 PHP 语句的发射器形态性损耗（bench 实测）：

| 项 | 全语句 | C 层核 | 损耗 | 损耗构成 |
|---|---:|---:|---:|---|
| array-read | 10.4ns | 2.51ns | ~7.9ns | FETCH_DIM_R 结果 TVal 装箱进 TMP 槽（2 store）→ ADD 槽重 load（2 load）→ $s wt（release 分支 + 2 store）→ INC/JMPZ 槽往返 |
| array-push | 55.8ns | 16ns | ~40ns | INC/JMPZ 槽往返 + ASSIGN_DIM 3 槽 load + helper CALL（cow/addref/append）+ 循环控制 |
| object | CI 6.38× | — | — | 同族（prop IC 命中后仍走槽交换） |

共同根因：**op 间值交换走帧槽**。emit_seq 逐 op 独立 lower，每 op 从槽
load 操作数、把结果 store 回槽——即使两个相邻 op 的值本可寄存器直达。

## 2. deps/ir 能力勘察（2026-10-31）

- **phi 支持 ✓**：`ir_PHI` opcode + `_ir_PHI_SET_OP`（ir.c:2817）存在。
- **IR_OPT_MEM2SSA (1<<22)** 存在——但针对**标量局部变量**（alloca 提升为
  SSA 值 + 自动 phi）。tphp 的帧槽是**堆数组 + 偏移访问**
  （`slots = ir_PARAM(...)` + `slot_addr = r_slots + idx*16`）——**mem2ssa
  无法提升数组元素访问**，不适用。
- **结论**：值直达必须走**发射器手工路径**（本设计），phi 用于多路径汇合点。

## 3. 方案：per-slot 值缓存（store-forwarding）+ 单态值直达

### 3.1 核心数据结构

```c
typedef struct { ir_ref tag, pay; } ValRef;      /* TVal 两字段 ref */
typedef struct { ValRef v; uint8_t valid; } SlotCache;
/* JitC 增： */
SlotCache *vcache;   /* 按 slot idx 索引，jit_lock 后按 n_locals+n_tmps 分配 */
```

### 3.2 命中条件（保守规则，失效遗漏 = 错值 = 语义 bug）

**读点**：`load_tag(j, slot_addr(j, idx))` / `load_pay(...)` 的所有调用处，
改为先查 `vcache[idx].valid`——命中直接用 ref，跳过槽 load。

**写点（登记）**——只登记**显式已知**的写：
- `store_imm/store_pay+store_tag` 的直接调用点（CONST/COPY/BINOP/INC 的
  dst lower）——**逐 lower 显式更新**（不做自动反查，addr ref 无法反解 idx）
- **已知写槽的 helper 返回后显式登记**：tphp_jit_array_append（base 槽
  可能因 cow/autoviv 更新）、dimic 命中（dst 槽）、CONCAT（dst）、
  incdec（src/dst）等——**全部 ir_CALL 调用点清单化**（~30 个），逐个标注
  写槽行为

**失效点（全清）**——任何"可能写任意槽"的事件：
1. **任何 ir_CALL 前全清**（helper 可能触碰任意槽）——随后按 3.2 写点规则
   重新登记已知写槽
2. **MERGE 进入新 BB / LOOP backedge**：跨控制流汇合的值需要 phi——批 A/B
   不做 phi → **汇合点全清**
3. **deopt RETURN**：RETURN 终止路径，自然失效
4. **store 到非 cache 追踪的槽**（含 PUT 类原语未标注处）——审计遗漏兜底：
   每个写点**必须**二选一（登记 or 全清），review checklist 强制

### 3.3 单态值直达（批 B 核心）

`$s += $a[$i]` 的值流改造：

```
现：DIMIC链 → 各命中分支 STORE tmp(tag,pay) → MERGE → ADD LOAD tmp
改：DIMIC命中 leaf → 产出 (tag_ref, pay_ref) → 写 vcache[dst]
    → MERGE（miss 分支 RETURN 终止 = 单活输入，无需 phi）
    → ADD 直接 ADD_I64(vcache[dst].pay) —— 零槽往返
```

- **单态前提**：DimIC 单态 site（packed 或单 hash 桶）= 单命中活路径 →
  值 ref 跨 MERGE 合法（另一输入已 RETURN 终止）
- **多态 site**（DimIC.cases 链多命中）：多活路径 → 值必须经槽（回退现行为）
  → **判据**：emit 期已知 site 的命中路径数（packed-only / 单桶快照 →
  单路径；否则槽回退）
- **packed 直取分支**天然单路径 ✓ —— array-read 主形态全覆盖

### 3.4 批 C（loop-carried，二期）

`$s` 的累加寄存器化需要 **loop phi**（LOOP_BEGIN 处值汇合）——
dstogov 的 LOOP_BEGIN 是 control 节点，loop-carried 值需手工构造
`ir_PHI(初始值, 回边值)` 并在 LOOP_END 闭合——**设计后单独立项**，
收益上限见 bench_arr_read.c 的 E 档（2.51→1.63ns）。

## 4. 与既有机制的交互

| 机制 | 交互 | 处理 |
|---|---|---|
| IR_OPT_FOLDING | 正交（vcache 是 emit 层，FOLDING 是 IR 层） | 无冲突；FOLDING 的 LOOP×MERGE bug（批 2-3）不受影响——两 bug 域不重叠 |
| DimIC 链 | 批 B 直接改造 dimic 命中 leaf 的产出方式 | 多态回退保持 |
| OSR | OSR chunk 同一 emit_seq——vcache 同样生效；OSR entry 时槽内容未知 → entry 全清 | 天然安全 |
| deopt（OSR_RET_PC/SEQ_PC） | RETURN 终止路径的 cache 不可达 | 无交互 |
| helper raise（setjmp 守卫） | raise → IR RETURN 0 → cache 不可达 | 无交互 |

## 5. 分批计划

**关键修正（2026-10-31，设计复勘）**：原批 A（纯基建）单独无收益——ADD 的
src 正是批 B 的 DimIC 输出，缺 B 则 A 空转；且 DimIC 的 packed 直取分支与
hash 链分支**静态双活**（即使运行期单态），MERGE 值合并绕不开槽——**值直达
必须绑定"模式折叠"**（折叠后 packed 分支内完成整条语句，不经 MERGE 交换）。

- **批 B'（packed 分支内联累加，~1 天，首刀）**：emit 期识别三连模式
  `FETCH_DIM_R(dst) → ADD($s, dst) → ASSIGN($s, tmp)`（$s += $a[$i]），
  在 DimIC 的 packed 直取分支内生成 fast 路径：
  `v=data[i]; IF(v.tag==INT && s.tag==INT){ s.pay += v.pay; store s }
   ELSE { 原三 op 慢路径（写槽） }`——packed 命中零 TMP 往返零 MERGE 后
  重 load；非 INT/$s 非 INT 走慢路径（语义零漂移）；hash 分支原样。
  验收：array-read 10.4→<8ns；test-osr/bench 双模对照。

**批 B' 控制流设计细节（2026-10-31 复勘）**：

1. **opcode 形态确认**（首步）：`$s += $a[$i]` 的实际三连
   （预计 `FETCH_DIM_R tmp, $a, $i` → `ADD t2, $s, tmp` → `COPY/ASSIGN $s, t2`，
   以临时探针 dump 实证为准）。
2. **fast/slow 双路径**（语义零漂移的关键）：
   - fast（packed 命中 && v.tag==INT && s.tag==INT）：
     `s.pay += v.pay; store s` —— 零 TMP 往返，pc 直接跳 pc+3
   - slow（v/s 非 INT）：**原三 op 走槽**（dst 写 + ADD/ASSIGN 照常 emit）
     —— **不可用 deopt 代替**：混合类型数组会形成"每迭代 deopt"循环
     （函数级 JIT deopt=整函数回解释器，hot 再编译同版本→死循环）。
3. **控制流机制**：fast 分支内 `pc += 3` 需要 stops 支持——把 pc+1/pc+2
   加入 emit_seq 的 stops，fast 分支 END 返回 TR_END(pc+3)，slow 分支
   落回主线继续 emit pc+1/pc+2，双路径在 pc+3 汇合。emit_seq 的 stops
   语义（循环出口）需扩展为通用"折叠消费点"。
4. **deopt 熔断旁证**：混合数组即使走 slow 也无 deopt——折叠版本与
   通用版本无状态切换需求（单版本内双路径）。
5. **工程量重估**：控制流重组 + 模式识别 + 双路径 emit ≈ 200 行，
   **1-1.5 天**（非顺手活，需完整双模测试矩阵：int/float/str 元素 ×
   int/float/str 累加器 × OSR/函数级）。

## 7. 批 B' 骨架落地记录（2026-10-31）

- **落地**：折叠检测（紧邻 ADD 消费 dst，compile.c A_VAR 实证两 op RMW
  形态）+ packed 分支 fast/slow 双线 + fwd 前向 MERGE（lb_pend 机制复用，
  emit_seq 主循环顶注入）。**op 形态修正**：$s += $a[$i] 是**两 op**
  （ADD $s, mid → $s，A_VAR RMW 直写 CV 槽），非设计稿假设的三连。
- **两次 IR 块结构修正**：①slow 分支误 END（连续 END 断链 ir_check=0）
  → slow 活线延续由 ifb-TRUE 块尾统一封；②溢出检查用 `ir_LT`（无
  ir_GE_BOOL/ir_LT_BOOL）。
- **实测（诚实）**：ir_check=1、238/238、sum 验证正确——**但 12.25ms 比
  未折叠基线 10.4ms 慢**：双线控制流 + fwd MERGE 税 > 省下的 mid 往返；
  **主项 $s 的 loop-carried 槽写未动**（批 C 域）。
- **结论**：批 B' 单独做负收益，**骨架保留**（结构正确），**净收益依赖
  批 C**（$s 驻寄存器后 fast 的 store 也消）——批 C 落地后重测再定去留。
- **批 A（基建并批 B'，顺手）**：vcache 结构随 B' 一并落（B' 的 fast 路径
  用局部 ref 即可，vcache 完整版可后置）。
- **批 B（DimIC 单态值直达，二期）**：3.3 节改造 + 单态判据。
  验收：多态回退路径测试。
- **批 C（loop-carried，二期单独立项）**：loop phi 构造。
  验收：E 档收益兑现（2.5→1.6ns）。
- **批 D（ASSIGN_DIM append IR 内联，~0.5 天，可独立）**：packed 条件直写
  data[n] 免 helper CALL（array-push 55.8→~45ns），与批 A 的 vcache 复用
  base 槽缓存。

## 8. 批 C slice-1 落地与证伪记录（2026-10-31）

**落地内容**（src/jit.c，tests/jit/test-fold-acc-001.php 9 场景）：

1. **fold_acc_scan 守卫**：循环体严格白名单（恰一个 FETCH+ADD RMW 对 +
   {INC,CONST} + 回边 JMP），任何嵌套结构/触碰 acc 的其他 op → 不激活。
   守卫通过保证：fwd merge 恒达、reconcile phi 恒支配回边、无嵌套循环
   pop 清掉激活态。
2. **loop phi 机制**（deps/ir 官方 idiom 实证可行，examples/0002-while.c
   + ir_check.c:198 对 loop phi 前向引用豁免）：LOOP_BEGIN 后
   `acc_phi = ir_PHI_2(IR_I64, init_pay, IR_UNUSED)`；回边
   `ir_PHI_SET_OP(acc_phi, 2, ...)` 补值——零 continue 时喂 fwd merge 的
   reconcile phi（`PHI_2(fwd_ssum, slow_pay)`，0 回边 load），有 continue
   时合并块内读槽兜底（权威值）。
3. **fast 线 pay-only store**：`$s` tag 恒 entry 值不被 fast 线触碰 →
   deopt/exit 槽恒 current，**无需 spill 基建**（完全去 store 的 spill
   基建留待批 C-2）。opay 读 = loop phi（零槽 load）。

**连带修 B' 潜伏 bug**：折叠 dst 槽误用 `aop->tr`（operand kind，OPD_CV=2）
当地址——OPD_CV=2 恰与 bench 闭包 acc 槽 2 重合被掩蔽；acc 在其他槽位的
函数会被写坏陌生 CV。修正为 `aop->ar` + kind/idx 语义全面核对（scan 配对
条件同步修正）。回归测试场景 1/2 专测 acc@slot0 与 slot3。

**实测证伪（5×5 同机交替，array-read 1e6）**：

| 版本 | ms | 说明 |
|---|---:|---|
| base（pre-B'） | **9.7** | min-of-5 稳定 |
| B'（骨架） | 10.8 | +1.2ns/iter 控制流税 |
| B'+C slice-1（phi） | **11.0** | phi 再 +0.2——未兑现寄存器驻留 |

**定性**：phi 值链（acc_phi→ssum→iter_phi→回边）跨循环体存活，本 RA（
dstogov linear scan）未给驻留寄存器——寄存器压力抵消省下的 2 次内存操作
（opay load + tag store），净 +0.2ns。**E 档（2.51→1.63ns）的前提是驻留
兑现**，本发射器层无法达成。

**处置（去留判据触发）**：折叠检测默认关断（`TPHP_FOLD_ADD=1` 重开），
机制代码+测试全量保留；239/239。**重开前提**（二选一）：① deps/ir RA
phi 驻留/合并增强（vendor 域，需勘察 ir_ra 的 phi 处理与 callee-saved
偏好）；② 转 dispatch 基线（P0-4 A6：67ns/轮 vs PHP 16）——array-read
单点已证边际递减，横向归因（§6）排序不变。

## 6. 风险清单

1. **失效遗漏 = 静默错值**（最高风险）：缓解 = 全清兜底 + 写点清单化 +
   每批全量 238 + bench 双模对照
2. **多态误判单态**：判据必须在 emit 期可证（DimIC 单态快照）；证不了回退
3. **IR ref 生命周期**：vcache 存的 ref 在 MERGE/LOOP 重排后是否被
   ir_schedule 重编号——**待验证**（批 A 先在直线段验证）
4. **代码体积**：值直达展开 = IR 指令变多——bench 体积对照
