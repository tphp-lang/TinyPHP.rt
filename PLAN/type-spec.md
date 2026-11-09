# 类型特化 / Loop Versioning（P0-17③ 最后一件：消每迭代 tag 检查）

状态：**批1/2/3 全部落地（2026-11-05/06，已提交至 e2679be）**——
TPHP_TYPE_SPEC（默认开，=0 关）。验收数据 §7a/§7b/§7c。
CI（run 42190592708 @ e2679be）：linux-x64 array-read 比值 3.42→**2.81×**。
**批4（kv 形态解锁）设计稿 v1 见 §8（2026-11-09，侦察批背书）——
4a 全部落地（4a-i 五件 + 4a-ii 直探/fwd）待提交：kv 本机 16.5→9.5
（A/B 双轮 −40/−41%，累计 −42%，超设计目标）、259×5（含 FWD/THIN 超集关）、
11 案矩阵三模绿；guard-fail 混合值 +156% 已定量并拍板接受记账（4d 留档
触发制）。4b/4c/4d 未动；WSL 第二口径与 CI 复核待办。**

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

## 7c. 批3 实测结果（2026-11-06）

**两件（§5）均已落地**：

1. **all_int 观察缓存**：TArr 加 `all_int` 标志——`tphp_jit_spec_guard` 验证 packed+全INT 后置 1；写路径（set/append/unset）清除；guard 首查标志免 O(n) 重扫。tarr_alloc_raw 初始化 0、tarr_cow 深拷贝。收益场景 = 重复循环入口 / OSR chunk 同数组免重扫。

2. **hash INT 键扩展**：DimIC hash 直达快路从 T_STR-only 扩展到 T_INT（splitmix via key_hash），同 4 探测上限、同快照门。kv-read 30-36→28-29ms（~5-10%）。

**验证**：254/254 全绿；bench 零回退。

## 7d. 刀15 take 瘦身实测结果（2026-11-09）——11-04 刀1 余税兑现

**切面**：spec_c 折叠路径的 `dimic_take` 三免（`TPHP_TAKE_THIN` 默认开，=0 关）：

1. **免 misses 清零 store**——spec site 该字段零读者：generic 门批2 不 emit、
   miss 计数 `dimic_miss(false)` 不计、C 侧仅 init（array.c:426）。
2. **免 addref 菱形 + vtag load**——元素恒 INT（guard g3 + 体内零写）→
   [STR,CLOSURE] 区间恒假，尾部 tag 直落 CONST INT。
3. **免 release dst 菱形**——**preheader 消灭**：guard 通过线对体内 FETCH dst
   槽（`spec_body_tmp`，spec_scan 输出恒等身份）恒发 `rel_dst_gate` + 置 UNDEF
   （每函数调用一次摊销），此后每迭代 dst=take 写入的元素值恒 INT；越界
   UndefinedError raise 不写 dst → deopt 重跑——dst 全程恒标量。
   thin 判定 = `spec_c && spec_body_tmp && op->ar == spec_body_tmp`（身份
   单一真源：字段非零 ⟺ preheader 已消灭 ⟺ 可 thin；cond 区 FETCH 的 dst
   不经消灭 → 保守原样三门，read 形态 cond 无 FETCH 零损失）。

**验证**：255×4（默认 / THIN=0 / TYPE_SPEC=0 / JIT=0——TYPE_SPEC=0 为超集
关闭，零影响）；新 `tests/jit/test-jit-takethin-001.php` 5 案三模逐字一致
（thin 主路径 / 越界异常与 thin 共存 / 异常后复跑 / guard-fail 轮转 /
STR 遗留 rc 稳定）；DEBUG=2 双级 ir_check=1、编译 size 1880→1744（−136B）、
IR −51 行（≈三门菱形量）。

**A/B（min-of-6 交替，双口径双轮）**：

| 口径 | array-read | 备注 |
|---|---:|---|
| Windows | 4.1→2.9（**−29.3%**）、复轮 4.2→2.9（**−31.0%**） | rmw +6.2% 复轮归零=噪声 |
| WSL linux | 4.0→2.8（**−30.0%**） | jsonagg/logstat 超带项专项 8 交替均为 on 带下移=噪声 |

**兑现链**：11-04 刀1 消融定案"两门动态≈0、税在静态布局（0.070 ≈ read 25%，
归因 block layout + 形态 guard）"——type-spec guard 提供形态证明后三门连同
冷段菱形整体消失，布局税回收。本机 read 自批1 起累计 6.5→5.7→4.1→**2.8**。
type-spec 系列（批1/2/3+刀15）全部落地；剩余 = FETCH 结构墙（MERGE/双活
旧案，spec 下路径可重估）。

## 8. 批4 设计稿 v1：kv 形态解锁（2026-11-09，待拍板）

**上游依据**：memory/2026-11-09.md「kv 结构底侦察批」三判据 + 根因闭合；
11-05 kv 侦察（刀6 已收键轮转税）。**设计先行（type-spec v1 先例），
本节拍板前不动代码。**

### 8.1 背景与账本

kv-read（CI 计分形态 = bench-vs-php 闭包 `$sum += $m[$keys[$k]]` 200k×8）
本机 16.5 vs PHP strongest 7.08 = **+9.4ms、比值 1.6×（CI 2.44×，剩余
最大 ln 杠杆项）**。侦察拆账（块校验闭合）：

| 块 | tphp | PHP | 说明 |
|---|---:|---:|---|
| lookup 本体（1.6e6 查） | **8.5 = 5.3ns/查** | ~3.8 | 最大块；**kv-1key（IC case0 一步命中）同为 5.3ns** ⇒ 与链深无关 |
| chain adds（7/iter） | 2.3 | ~0.7 | tag 检查税 |
| floor + outer/取键 | 5.7 | ~3.2 | 循环控制+packed 取键 |

**根因 = 通用发射税（缺 int 证明）**：snapshot 门 + take 双菱形
（addref/release/misses store）+ dst 槽内存往返 + ADD/INC/cond tag 检查——
**刀15/16 的消税机器（take_thin / fetch_fwd）与批1/2 的检查点全部锁在
spec 门后，kv 一行没吃到**（spec_scan 拒因 = `2nd fetch`/`CONST`，零开销）。

**预期**：kv-full 16.5 → **~10-11.5（−30~40%）**，本机比值 1.6→~1.1；
CI kv 2.44×→~1.5-1.7×。

### 8.2 目标形态与范围

- **批4a（首刀）= CI kv-inner 形态**：循环体 = `FETCH1($keys, idx cv) →
  FETCH2($m, key=tmp) → ADD(acc, val tmp) rmw → INC(idx)`（单回边、
  preguard 已收双 base，实测 hdr=48 n=2）。$m = hash（非 packed）。
- 不在 4a：多 fetch 链（cv8/const 外层，8 fetch+7 链加）= **批4b**；
  1key（键 CV 恒 STR）/ CONST 字面量键 = **批4c**；FE/foreach、体内
  CALL 等白名单外形态照拒（零开销不变）。

### 8.3 机制设计（批4a 五件）

1. **spec_scan 扩形（2-fetch 链）**：
   - FETCH 白名单放宽为**恰 2 个**：fetch1（`t2==OPD_CV`，a2=idx 候选、
     a1=key_base）、fetch2（`t1==CV a1=value_base`、**`t2==TMP 且
     a2==fetch1.ar`** 即键=前一取值）；
   - **三 op 强制相邻**：`fetch2 == fetch1+1` 且 `add == fetch2+1`
     （编译器对 `$m[$keys[$k]]` 恒相邻；不相邻即拒——防 spec_tmp 时序
     穿帮：F1 尾置 spec_tmp=键 dst，若 F2 未紧随覆盖而中间 op 消费
     spec_bit_int，会把 STR 键当 INT 证用）；
   - ADD 判据不变（rmw 配对 acc，**紧邻 fetch2**）；INC 判据改为
     `a1 == fetch1.a2`（idx 对 fetch1 的键槽）；
   - 身份输出扩：`acc/idx` 同前 + `tmp=fetch2.ar`（值 dst）、
     `key_tmp=fetch1.ar`、`base=fetch2.a1`（guard 目标）、
     `key_base=fetch1.a1`；全身份两两互斥（镜像现有 identity alias 拒）；
   - 写点约束扩到 **base 与 key_base 双槽**（pre_op_writes，body 零写
     ⇒ 数组结构/元素类型循环期不变——沿用 §3 证明面）；
   - base∈preguard cand 判据对两 base 各查一遍。
   - **form 标记 = 2-fetch ⇒ ht**（guard 目标按此选 helper）。代价：
     packed 基的 2-fetch 形态（如 int 键套 int 键）会 guard 失败 →
     交解释器；该形态**今天同样无 spec**（scan 即拒），但今天走通用
     JIT、4a 后走解释器 = 新回退面（§8.7-1 专项验收）。
2. **guard 扩展（vm.c 新 helper）**：`tphp_jit_spec_guard_ht(base)` =
   `T_ARR && !packed && 全活跃桶值 INT`——`all_int` flag 首查免重扫
   （批3 同款）；扫描 = `buckets[0..bpending)` 跳 tombstone
   （val_off==UINT32_MAX），值取 `data[val_off]`。emit 侧按 form 选
   helper 地址，acc/idx INT 检查与失败 `RETURN(OSR_RET_PC)` 与批1 逐字
   同构（fn 级 pc0 重跑、OSR chunk entry = 批2 语义，继承既有协议）。
   - **flag 语义扩**：value.h 现注释为"packed 且全元素 INT"，4a 扩为
     "布局无关：全活跃值 INT"——失效面已覆盖（array.c 195/289/446 +
     mem.c 180 任何写清位、布局破坏清位、深拷贝继承），实现时审计
     packed↔hash 转换点（446 布局破坏）确在清位即可。
3. **FETCH2 ht 直探结构（spec_ht 结构消除）**：FETCH emit 在 `spec_c`
   （packed，批2）旁新增 `spec_ht` 分支（同为 `spec && pi>=0 &&
   base==spec_base && form==ht`）：
   - inv 外层与 IC 快照门（arr/bk/bpending）、case0、case1-7 链、generic
     8-fail 门**整段不 emit**（guard 已证 ARR+结构稳定：体内零写+无别名
     ⇒ buckets/hmask/bpending 循环期不变——**无需 IC 快照**，直探读
     g.a 即可）；
   - 体内：`IF kt==T_STR { hash 惰性（h==0 → tstr_hash CALL，lvl4 同款）
     → slot=h&hmask → ≤4 步 sequential probe（镜像 array.c 快路② +
     刀6 的 tombstone/absent 终止判据）→ HIT: leaf=data[val_off]
     | MISS: dimic_miss helper 兜底 } ELSE { dimic_miss helper }`；
   - **结构消除非降级**：probe 超步数/非 STR 键/absent 全部落既有
     tphp_jit_fetch_dim_r helper（IC fill/raise/norm 语义逐条不变）。
4. **thin/fwd 与 spec_c 解耦**（本批关键改动点）：
   - 现条件 `fwd_on = spec_c && spec_body_tmp && ar恒等`（jit.c:3763）与
     take thin（:3784）均绑 `spec_c`（=packed 结构证）——新增
     `spec_ht` 作为**同级的"元素恒 INT"证明源**：条件改
     `(spec_c || spec_ht) && 身份恒等`；
   - **fwd phi 挂入 ht 直探块**（刀16 同构）：hit 线 `LOAD pay`（免 take
     16B store）、miss 线 `load dst`（helper 成功写值的深碰撞线可达、
     值亦真；absent 线恒 raise 实死）→ MERGE `PHI` → 紧邻 ADD 消费
     （lower_binop 现有 kind+idx 检查点零改动）；
   - **preheader 消灭**：仍只针对 fetch2 值 dst（元素恒 INT 证 → 三门
     免，刀15 原样）；fetch1 键 dst 元素类型**不证**（$keys 值可任意，
     不引 all_str）→ take 三门保留原样、不入消灭名单（键走 fwd 前传
     属 4b，届时连 take 一并免）；实现上 `spec_body_tmp` 单槽语义不变，
     4b 多 fetch 时再扩槽组；
   - **ADD/INC 检查点零改动自动生效**：FETCH 尾置 spec_tmp（:3881）、
     lower_binop 双 INT 快线、lower_incdec spec_i 均已在，scan 放行即点火。
5. **lower_cmp 检查点（顺带，可独立拆出先做）**：cond 区 LT（`$k < 8`、
   read 的 `$r/$i` 边界）现**无** spec 检查（lower_cmp:1696 无条件
   load tag）——补 `spec_bit_int` 免 tag load/both cmp。read+kv 通吃。

**明确不做（沿用 §2 决策）**：双版本循环体（guard-fail 回退通用 JIT 需
emit 两份循环体，emit_seq 单遍约束不变）；key 恒 STR 的 all_str 证明
（4a 用 STR 分派保底，见 8.6-4c）。

### 8.4 安全判据

- 假设面与批1-3 完全同族：体内零写（写点约束扩 key_base）+ tphp 无
  引用别名 ⇒ $m 结构（buckets/hmask/bpending）与元素值类型循环期不变；
  flag 失效由写路径清位兜底（审计点 = packed↔hash 转换）。
- fwd 不逃逸：值 pay 只进紧邻 ADD；键 pay 不前传（4a 键仍走槽+take，
  key-fwd 属 4b）。deopt 物化复用刀16（溢出 RETURN 前补写 fwd 槽）。
- ht 直探与 C 快路②逐条同判据（tombstone 跳、T_NULL 终止、≤4 探测、
  hash 首比）——语义对拍由 helper 兜底保证（探不到的全部回 C）。
- guard-fail = 交解释器（批1 既有策略原样继承，含 pc0 重跑协议——
  非本批新增面，见 §7a"溢出 RETURN 解释器重跑"）。

### 8.5 与既有机制交互

| 机制 | 交互 | 处理 |
|---|---|---|
| preguard | 双 base 已在 cand（实测 n=2）；inv/pk/n/dt phi 直接复用 | 零改动 |
| 批2 spec_c（packed 消除） | 与 spec_ht 并列分支，互斥（form 由 scan 定） | 原样保留 |
| 刀15 thin / 刀16 fwd | 条件从 spec_c 单证扩为 `spec_c‖spec_ht` 双证 | 解耦改造 |
| 刀6 链内线性探 | spec_ht 点整段不发链；非消除点（cond 区 FETCH 等）刀6 照常 | 零干扰 |
| OSR（批2） | guard emit 已在 chunk entry；ht helper 换地址即可 | 零改动 |
| fold/B'（默认关） | fwd gate 既有 `!fold_add` 前置不变 | 零改动 |
| SlotGate/刀3 | 槽级写者分析与 kv 无关（main 全保门照旧） | 零改动 |
| IC 统计（TPHP_IC_STATS） | spec_ht 直探不经 ic 快照；miss 走 helper 照常计数 | 记口径差异 |

### 8.6 分批计划

- **批4a（首刀）**：§8.3 五件 + 新矩阵测试；验收 §8.8。
- **批4b**：key-fwd（fetch1 pay phi 直供 fetch2 键位，免 STR take+槽往返
  ≈1ms；需 rc 论证：键只被探针身份比较、不逃逸）+ 链式 fwd
  （binop→binop phi，cv8/const 多 fetch 形态的 spec_scan N-fetch 扩形）。
- **批4c**：1key（键 CV 恒 STR——guard 加一条槽 tag==STR 检查）与
  CONST 字面量键（lit 类型编译期即知，免 kt 装载）形态。
- **批4d（观察触发）**：guard-fail 回退面治理——若 8.7-1 专项实测回退
  显著，选项 = 静态证明线（literal ARRAY_INIT 溯源免 helper）或其它
  非双版本方案，另立小设计稿拍板。

### 8.7 风险清单

1. **guard-fail 解释器降级（新回退面，一级）**：packed 基 2-fetch、
   混合值/float/字符串值 hash map 的 kv 形态循环——今天走通用 JIT，
   4a 后交解释器（若命中）。验收 = 专项探针（混合值 map 循环
   TYPE_SPEC on vs off min-of-6）+ 全表 A/B；显著回退 → 4d 提请拍板。
   预判命中面小（bench/kv 形态全 INT 值；logstat grp/jsonagg grp 是
   packed int 键单 fetch = 读形态不走 4a 面）。
2. **ht 直探语义对拍**：tombstone（unset 中间键）、数组重建（bpending
   非零/扩容）、absent 键 raise、>4 深碰撞——全部 helper 兜底，但
   直探内判据写错=错值：矩阵测试必含四案 + 四模对拍。
3. **fwd+thin 在 ht 路径的 dst 生命周期**：slot UNDEF → helper 深碰撞
   线写值（thin 免 release 的前提是恒 INT ✓ guard 证）；phi miss 线
   load 可达性与刀16 同论证。
4. **emit 双上下文漏点**：spec_c 与 spec_ht 并列后，FETCH 检查点清单
   化 + review checklist（镜像 rel_dst_gate 写点纪律）；漏点方向 =
   少发检查（错值），由矩阵+全表兜底。
5. 代码体积：ht 直探 ≤4 步 vs 现 8-case 链（每 case 3 装载+cmp）——
   预计持平或减，DEBUG=2 size 记账验收。
6. guard 每次进入 preheader 执行（kv = 每外层迭代一次 ×200k）：
   flag 首查免扫（批3），残余 = helper 调用+ARR/packed/flag 判 ≈
   每次几条指令；仍失败形态（8 桶扫描 ×200k）= 少数 ms 级上界，实测专项。

### 8.8 验收计划

- **测试**：`tests/jit/test-jit-typespec-kv-001.php` 新矩阵（≥9 案）：
  kv-full 主路径值轮转 / hash 混合值 guard-fail 语义（=解释器结果
  逐字同 PHP）/ unset 中间键 tombstone / 数组重建后再入 / absent 键
  raise+catch / >4 深碰撞 / int64 溢出 RETURN 重跑 / STR 键 rc 稳定
  ×1e5 / OSR 顶层形态（kv-const 同构）。**全量 259×3**（默认 /
  TYPE_SPEC=0 / JIT=0）+ TAKE_THIN=0、FETCH_FWD=0 超集关复跑。
- **A/B**（同二进制 min-of-6 交替，双口径 Windows+WSL）：kv-full
  目标 **−30~40%**；全表零回退（超带项 8 交替复核惯例）；
  guard-fail 混合值专项（风险1 定量）。
- **结构观测**：DEBUG=2 kv 循环 IR 门计数/size（直探 vs 链）；
  IC-STATS helper 率（现 27% → 直探下 probe 命中、miss 率实测）。
- **CI 投影**：kv tphp 16.5（本机）/22（CI 带）→ 比值 2.44×→~1.5-1.7×；
  主口径 clean11 ln 杠杆约 −0.6/13。
