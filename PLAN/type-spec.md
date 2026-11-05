# 类型特化 / Loop Versioning（P0-17③ 最后一件：消每迭代 tag 检查）

状态：**批1 已实施（2026-11-05，待提交）**——TPHP_TYPE_SPEC（默认开，=0 关）；
设计稿 v1 同日拍板。批1 验收数据见 §7a，批2 见 §7b。批3 未动。

历史：**设计稿 v1（2026-11-05，用户拍板"先设计"）**——规格、机制选型、风险、分批计划。
上游依据：emit-ssa.md §③ 类型特化；read 减法定案（memory/2026-11-05.md 主刀轮收口）。

## 1. 目标与账本

read（array-read 1e6 闭包）现状 6.5ms ≈ 6.5ns/iter，PHP strongest 1.1ns。减法拆账：

| 段 | ns/iter | 状态 |
|---|---:|---|
| FETCH（链/边界/装箱） | 3.5 | 结构墙（emit-ssa §5 MERGE/双活），fold 唯一解已封存 |
| 循环控制 | 1.7 | ~0.25ns/条近硬件效率，无肉 |
| 累加 | 0.3 | RA 已优化殆尽 |
| **检查类（tag cmp/sete/常量物化）** | **~1-1.5** | **本立项靶子** |

asm 证据（def 版 0x4d9 循环体）：`$s` tag cmp+sete、`v` tag cmp+sete、`$i` tag cmp+jne、
DimIC key tag 检查、dst tag 每迭代重写常量——每迭代 ~10-15 条检查/物化指令。
**PHP tracing JIT 做到 1.1 的手段即观测类型后特化消检查**——本项是与 PHP 的真实差距来源之一。

预期：read 6.5 → **~5.0-5.5**（CI 比值 4.45× → ~3.5-3.8×）；对全部 preguard 白名单循环
通用（read/rmw/push 主循环形态皆可受益）。诚实上限：边界税与 TVal 结构仍在，非 1.1 追平件。

## 2. 核心机制（单版本 + preheader guard + deopt 兜底）

**不做双版本代码**（emit_seq 单遍无法 emit 两份循环体）。采用 baseline JIT 惯用的
speculation 形态：

```
（循环 preheader，LOOP_BEGIN 之前）
  g1 = base 是 T_ARR                     ← 复用 preguard 的 inv 外提
  g2 = packed                            ← 复用 preguard 的 pre_pk
  g3 = all_int(base)                     ← 新 helper：O(n) 扫全元素 tag==INT（每调用一次）
  g4 = slot($s) tag==INT && slot($i) tag==INT
  IF (g1 && g2 && g3 && g4) {
      LOOP_BEGIN ... 特化循环体（跳过全部 tag cmp/sete 与 dst tag 重写）
      LOOP_END ... 出口照常
  } ELSE {
      ir_RETURN(resume = 循环起始 pc)     ← 解释器接管（既有 OSR_RET_PC/重启语义）
  }
```

- **特化体 emit**：`j->spec` 上下文标志，进入循环 emit 时置位（guard TRUE 分支内），
  emit 各检查点按标志跳过：
  - FETCH：`g.kt == JT_INT`（key tag）、`vtag == INT`、`otag == INT` 的 cmp/sete 免；
    dimic_take/ASSIGN **只写 pay、免 dst tag store**（恒 INT，tag 不变；批C pay-only
    store 同款论证）。
  - ADD/INC：$s/$i 的 tag load 与 cmp 免（值恒 INT）。
  - 溢出检查**保留**（成本实测免费（B−C = 0.3 含它），且 float 降级是真运行时分支）。
- **guard 失败出口** = ir_RETURN 到循环起始 pc：解释器从循环条件重进（preheader 处
  尚未执行循环体 → 零双跑、零死循环——与 §5.2 "每迭代 deopt 死循环"警告的区别：
  guard 每调用只评估一次，失败即整段交解释器，无"编译同版本反复进即 deopt"）。
- **all_int 扫描成本**：1000 元素 × (load tag+cmp) ≈ 数百 ns/调用，1e6 迭代摊销
  <0.001ns/iter；多次调用的真实负载 = 每调用 O(n)，v1 接受，v2 可做观察缓存
  （site 记"上次数组指针 + 全INT 扫描通过"快照跳过重扫）。

## 3. 适用面与安全判据（全部复用既有机制）

- **循环准入 = preguard_scan 判据原文复用**：单回边、单前驱入口、无外部跳入、
  **体内纯 op 白名单（读任意 + 写自身标量槽，无 CALL/数组写/RETURN/THROW/FE）**
  ——体内无数组写 + tphp 无引用别名 ⇒ 进入循环后 base/元素类型不可变，
  g3 扫描结论整个循环期有效（假设破坏不可达）。
- 元素被改写的唯一途径是体内 store（白名单排除）或引用逃逸（不存在）；
  UNSET/间接写同理。
- guard 失败形态（hash 数组/混合类型/$s 非 INT）→ 每调用一次 deopt 成本 ≈
  preheader 几条 cmp + 返回切换，可接受且**无重入**。

## 4. 与既有机制交互

| 机制 | 交互 | 处理 |
|---|---|---|
| preguard（外提 inv/pk/n/dt） | g1/g2 直接复用外提值；guard 菱形须在 preguard 菱形**之前或合并**（支配序） | 合并为单一 preheader 菱形 |
| fold/B'（默认关） | spec 下 fold fast 线的双 INT 检查同免 | 封存态不动；spec 开时 fold 仍关 |
| OSR | spec 循环的 OSR chunk：批2 已落地（§7b）——guard emit 移入 chunk entry 块（preheader 菱形后、LOOP_BEGIN 前），失败 RETURN(backedge_cur)=解释器执行回边 JMP 续跑，entry 前零副作用 | 已落地 |
| deopt（helper raise） | 特化体的 pay-only store：resume 时槽 tag=旧 INT=真值 ✓ 与批C 同论证 | 已论证 |
| DimIC 快照 | g3 覆盖 packed+全INT；IC 链本身仍发射（key tag 检查免但结构在——v1 只免 cmp 不动链结构）→ 批2 已落地（§7b）：spec+guard 证下 ifa/pcond/链整段不 emit | 已落地 |

## 5. 分批计划

- **批1（本立项首刀）**：fn 级 read 形态最小闭环——
  1. preheader 菱形 emit（g1-g4 + all_int helper + FALSE→RETURN resume 循环头 pc）；
  2. `j->spec` 标志 + FETCH/ADD/INC 三处检查点跳过 + dst tag 免写；
  3. 判据复用 preguard_scan（spec 独立开关 TPHP_TYPE_SPEC=0 关）；
  4. 验收：read ≥−1ns（6.5→≤5.5）、246×3、全表零回退（特化错值由
     tests/jit 语义对拍矩阵兜底：int/float/str 元素 × int/float 累加器 ×
     首次调用即混合类型 × hash 数组 guard-fail 路径）。
- **批2**：DimIC 链结构在 spec 下的消除（免 bound 分支外的层叠 cmp）、OSR chunk 入口 spec。（已落地，实测见 §7b）
- **批3**：all_int 观察缓存（IC 快照跳过重扫）、hash INT 键扩展。

## 6. 风险清单

1. **假设破坏 = 静默错值**（最高）：体内零写白名单 + 无引用别名为证明面；每批全量
   246 + 双模语义对拍（JIT=0 逐行）兜底。
2. **guard-fail 高频形态**（混合数组反复调用）：每调用一次 preheader+RETURN 成本，
   无重入；bench 矩阵含该路径。
3. **emit 双上下文复杂度**（spec 标志漏点 = 少检查错值）：检查点清单化 + review
   checklist（镜像 rel_dst_gate 的写点清单纪律）。
4. **代码体积**：特化体 = 现体减检查，不膨胀。

## 7. 验收与观测

- A/B：TPHP_TYPE_SPEC 默认开 / =0 关，min-of-6 双口径（Windows + WSL linux）。
- 探针：%TEMP%\tp_read_ablate.php 减法矩阵复跑（A/B/C/D 形态）。
- DEBUG=2 机器码落盘对比：循环体 `cmp/sete` 计数前后差（直接指标）。

## 7a. 批1 实测结果（2026-11-05）

**实现落点**：vm.c `tphp_jit_spec_guard`（ARR+packed+全元素 INT 一体查）；
jit.c JitC spec 字段 + `spec_scan`（结构判据复用 preguard_scan，形态=恰
FETCH+ADD-RMW 配对+INC/DEC，身份互斥+写点约束）+ push 处 guard emit
（IF(!g) TRUE→RETURN(0)，FALSE 线续 emit=OP_SEND_CAP 形态；OSR chunk 不特化）
+ 三检查点（lower_binop 双 tag/慢线菱形、lower_incdec isint/慢线、FETCH pcond
kt）+ 溢出分支 RETURN(0) 化 + spec_tmp 跨 op（FETCH 尾设/ADD 消费即清）+
pc 出体自动失效。`tests/jit/test-jit-typespec-001.php` 9 案矩阵。

**验证**：247×3 全绿（默认/spec=0/JIT=0）；矩阵三模逐字一致（含 int64 溢出
→RETURN→解释器重跑=double、guard-fail 元素 float/str/非 packed/acc 入口
非 INT/编译后类型轮转、DEC 倒序、空数组 n=0、槽0 acc）。
**顺带修复既有缺陷**：generic ADD/INC 溢出分支 `BITCAST_D` 位重解释得 NaN
→ `ir_INT2D` 数值转换（矩阵溢出案首暴露；spec=0 模式原样 NAN 与解释器发散）。

**A/B（min-of-6 同二进制交替，全表零回退两轮复核）**：

| 口径 | array-read | 其余全表 |
|---|---:|---|
| Windows | 5.7→5.4（**−5.3%**，两轮一致） | ±1% 内 |
| WSL linux（主口径） | 6.2→5.7（**−8.1%**） | ≤±1.6%（rmw 首轮 +6.7% 复轮归零=噪声） |

**asm/IR 对比（DEBUG=2，read 闭包单案）**：on 6208B vs off 6256B；全文
EQ 计数 102 vs 104——spec 消 3 个检查点 EQ + 慢线菱形（call 框架），
动态 ×1e6 ≈ **−0.5ns/iter**。

**诚实记账**：read 未达 −1ns/iter 目标（实收 −0.5）——asm 证据里"每迭代
10-15 条检查"的其余部分（dst tag 重写、dimic 链层叠 cmp、bound 分支）
按设计属批2（DimIC 结构消除）；检查类剩余肉记 backlog。
批1 判据附带收益：`spec scan` 拒的形态（kv/push/rmw/外层 r 循环）零 emit
差异=零开销（scan 纯编译期）。

## 7b. 批2 实测结果（2026-11-05 晚）

**两件（§5）均已落地**：

1. **DimIC 结构消除**：spec 作用域内 FETCH 满足
   `base == spec_base && key 恒 INT（spec_bit_int）` → guard（纯读 helper，
   与 preguard 菱形同块、无中途写 ⇒ inv≡真、packed≡真）已证 ARR+packed+
   全INT → `ifa` 外层、`pcond` packed 门、非 packed/非数组回退与 IC 层叠链
   **整段不 emit**，只留 `[bound→取值 | 越界→miss 兜底]` 单菱形（idx<n 不在
   guard 面，越界照走 v1 helper——结构消除非降级，miss/raise/deopt 语义
   逐条不变）。`spec_base` 新字段防 cond 区异 base 候选误折（无 guard 证
   不折）；kt 装载随之免发。未折叠形态（key 非恒 INT 等）原结构零改动。
2. **OSR chunk 入口 spec**：去掉 `!j->osr` 闸——guard emit 在 chunk entry
   块（preheader 菱形后、LOOP_BEGIN 前），失败 RETURN(backedge_cur) =
   解释器执行回边 JMP 续跑；entry 前零副作用（菱形纯装载、guard 纯读），
   chunk 未执行体代码 → resume 干净，与 fn 级 preheader 语义一致。

**验证**：247×3（run1 一次 test-net-http-tls 环境挂起 0CPU——独立双模
复跑秒过、run2/3 全绿，非本批回归）；release 族探针双模逐字一致；
DEBUG=2 双级 guard 触发：fn 级 `osr=0`、read 基准 OSR chunk `osr=1`。

**A/B（Windows min-of-6 同二进制交替，TYPE_SPEC=1 vs 0）**：

| bench | spec=1 | spec=0 | 结论 |
|---|---:|---:|---|
| array-read | **4.0ms** | 5.5ms | **−27%**（结构消除+OSR 特化，主靶） |
| kv-read | 18.0 | 18.1 | 平（形态不符 spec_scan，零 emit 差异） |
| array-rmw | 14.5 | 14.5 | 平 |
| fib | 7.5 | 7.5 | 平 |
| call | 10.4 | 10.5 | 平 |

**结构指标（DEBUG=2 OSR chunk 尺寸）**：spec=1 **865B** vs spec=0 **5640B**
（−85%，inv/pcond/链/generic 回退全部消失）。批1 记账的"批2 剩余肉"
（链层叠 cmp+外层菱形）即此。

**观测/后续**：批3（all_int 观察缓存、hash INT 键扩展）未动；WSL/CI 口径
待 push 后由 perf 流复核。
