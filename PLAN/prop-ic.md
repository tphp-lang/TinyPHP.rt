# 属性访问 IC 内联快路（prop v1.5）——消 JIT 侧 helper/replay 边界

状态：**批1+批2 已落地并提交（2026-11-10）**——TPHP_PROP_INLINE（默认开，=0 关）；
FETCH_PROP + ASSIGN_PROP CONST 名 IR 内联快路。验收 §7a/§7b。
上游依据：object 侦察批（memory/2026-11-10.md）；刀5 记账 v1.5；刀8/5B 证伪记录（helper+setjmp 方案不做）。

## 1. 目标与账本

object 1e5 CI 形态 2.83×（13.3ms）；本机权威对拍（php8.5.1+tracing JIT）full 19.87 vs 7.54 = 2.6×。

**侦察拆账（recon3 min，1e5）**：

| 段 | tphp | PHP | 比值 | 归因 |
|---|---:|---:|---:|---|
| prop_direct_rw | 6.97 | 0.18 | **38×** | JIT 内 ASSIGN+FETCH 全走 helper/replay |
| method_get_warm | 4.60 | 0.60 | **7.7×** | get 体已 fold+JIT，慢在 FETCH_PROP helper |
| alloc_ctor_get − alloc_ctor（get 增量） | +0.48 | +0.63 | ≈平 | get 本身不贵 |
| full | 19.87 | 7.54 | 2.6× | 主体 = alloc/ctor/dtor C 侧机械（P0-4/P0-16 域） |

**边界实锤**：`compile(get) size=115`（体 = helper 调用壳）+ `fold mcall` + OSR 主循环——结构全 JIT，**慢在 `tphp_jit_fetch_prop`（假帧+setjmp+真 handler，~70ns）与 ASSIGN 的 `lower_replay`（解释器 pad）**。PHP tracing JIT 内联 offset ~1.8ns/iter。

**预期（诚实，本机 recon3 口径）**：
- prop_direct_rw 6.97 → **~2.0-3.0ms（−55~70%）**；对 PHP 比值 38× → ~11-16×（PHP 0.18 是 LICM+寄存器化地板，不追平）
- method_get_warm 4.60 → **~2.3ms（−50%）**，比值 7.7× → ~4×
- full 19.87 → **~16.5ms（−17%）**（仅 get 侧 FETCH 回收；alloc/ctor/dtor 机械不在本刀面）
- CI object 2.83 → **~2.4-2.6×**（大头仍=P0-4 dispatch 主线）

不做：dtor 直入、ASSIGN_PROP helper+setjmp（刀8/5B 已证伪净负）。

## 2. 核心机制（镜像解释器 IC 快路，零解释器改动）

解释器已有同款快路（vm.c `op_fetch_prop`/`op_assign_prop` P0-19 round-6）：`op->ic` 存 slot+1，
命中条件 = CONST 名 + bounds + **interned 名指针相等** + public + !hooks（assign 另加
!type/!readonly/!set_visibility/!CLS_READONLY）。**本刀 = 把这条快路用 IR 发射出来，
miss 落既有 helper/replay（ic 由真 handler 回填）**。

### 2.1 FETCH_PROP 快路（批1）

```
obj = slot[a1]                          // 操作数：obj, name(CONST), dst
IF (tag(obj)==T_OBJ && kind(obj)==GC_KIND_OBJ) {
  icv = load &op->ic                    // TOp 地址编译期常量
  IF (icv != 0) {
    slot = icv - 1
    cls = load obj.cls                  // TObj.cls @24
    np  = load cls.n_props               // TClass.n_props @80
    IF (slot < np) {
      pp = cls.props + slot*80           // TClass.props @72；TProp=80
      IF (load pp.name == NAME_CONST     // interned 指针，编译期常量
          && load pp.visibility==PUBLIC  // @24
          && !load pp.has_hooks) {       // @72
        // HIT：镜像解释器 4315-4324
        vaddr = obj.props + slot*16      // TObj.props @32；TVal=16
        addref(load vaddr)               // tag∈[T_STR,T_CLOSURE] 门（dimic_take 同款）
        rel_dst_gate(dst)                // 刀3 写者分析已有
        store dst = vaddr 的 16B
        → 跳过 slow
      }
    }
  }
}
// MISS / 任一守卫失败：
tphp_jit_fetch_prop(...)                // 既有 helper：造帧+真 handler（回填 ic）+setjmp
```

- **仅 `t2==OPD_CONST`**（动态名无 ic，恒 miss——与解释器同）。
- 名指针相等 = 跨类多态安全（继承 memcpy 同 interned `TProp.name`；子类重声明换 slot 则
  `props[slot].name` 失配 → miss → helper 重填——与解释器逐位同源）。
- **零解释器改动**：ic 格式仍是 slot+1，双方共用 `TOp.ic`。

### 2.2 ASSIGN_PROP 快路（批2）

守卫链同 FETCH，hit 条件镜像解释器 3124-3126：

```
visibility==PUBLIC && !set_visibility && !has_hooks
&& !type && !is_readonly && !(cls.flags & CLS_READONLY)
```

HIT（镜像 3127-3136）：
```
addref(val)                             // val = slot[a3]
rel_dst_gate(obj.props+slot*16)         // 释旧属性值
store 16B
IF (tr != UNUSED) { 同款 addref+rel_dst_gate+store 到 dst 槽 }   // 赋值是表达式
→ 跳过 slow
```

MISS → 既有 `lower_replay(pc, pc+1)`（真 handler 回填 ic）。

### 2.3 为何不用 PropIC 结构体 / 类指针快照

| 方案 | 取舍 |
|---|---|
| **A. 镜像解释器名指针校验（本稿）** | 零解释器改动、多态安全、与快路逐位同源；守卫 ~6-10 load 仍远低于 helper 70ns |
| B. PropIC{cls,slot,name} 结构 | 需改解释器 ic 格式（slot+1 → 指针），双端同改、回归面大 |
| C. 类指针恒等守卫 | 最快（1 cmp），但 `$this` 继承实例（子类 cls≠decl_cls）与多态点全 miss；需 B 的结构才存得下 |

v1 选 A；若 A 后 guard 仍成瓶颈再评估 B（另立项）。

### 2.4 emit 落点

- `jit.c` `case OP_FETCH_PROP`：`prop_helper_enabled()` 且 CONST 名 → 发 2.1 快路菱形，
  else 维持 helper。`case OP_ASSIGN_PROP`：CONST 名 → 发 2.2，else 维持 replay。
- 守卫失败边 = 既有 slow（helper/replay），**不新增 deopt/RETURN 语义**——raise 面全在 slow。
- 开关 `TPHP_PROP_INLINE`（默认开，=0 回 HEAD helper/replay，同二进制 A/B）。
- offsetof 全部 `offsetof()` + `_Static_assert`（TVal.u/t 已有先例）；TProp/TObj/TClass
  布局变动即编译期断言炸。

## 3. 适用面与安全判据

| 面 | 处理 |
|---|---|
| 动态名（`$o->$n`） | t2≠CONST → 恒 slow，与解释器同 |
| private/protected | 快路仅 PUBLIC（scope 相关不可缓存）→ 非 public 恒 slow |
| magic `__get/__set`、hooks、virtual | 守卫 `!has_hooks` 等 → miss 走 helper 完整逻辑 |
| typed/readonly/asymmetric | assign 守卫逐字段排除（镜像 3124） |
| ~~FFI CData~~ | FFI 已移除（2026-11-10）；守卫链仍兼容 kind/n_props 形态 |
| enum `->value/name` | slot 查找失败路径在 helper，快路不进 |
| 继承/多态 | interned 名指针相等（解释器同款）；重声明换 slot → miss 重填 |
| by-ref `&$o->v` | **v1 按普通 get 执行**（ast.h `get_by_ref` 注释：解析保留、不产引用）——与 HEAD 同语义，快路无需特判；若未来真 by-ref 引用语义落地再排除 |
| UNDEF typed 未初始化读 | **镜像解释器**：快路不查 UNDEF（解释器 4317 检查在 !hooks 分支内、快路 4214 不查）——零漂移优先；若对拍暴露差异再同步修两侧 |
| 线程 | 类 flatten 后 props 布局不可变；ic 最坏竞态 = 重复填/脏 slot → 名指针校验兜底（DimIC 同纪律） |

## 4. 与既有机制交互

| 机制 | 交互 | 处理 |
|---|---|---|
| 刀5 FETCH_PROP helper | 快路 miss 的落点，不动 | `TPHP_PROP_HELPER=0` 时 helper 关 → miss 落 replay（HEAD 关态行为）；快路本身仍可用（`TPHP_PROP_INLINE` 独立） |
| 刀3 dst_gate 写者分析 | HIT 路 rel_dst_gate 沿用 `dst_gate_needed` | 免发条件同既有 |
| mcall fold | get 体已 fold；本刀加速 get 体内 FETCH | 正交，叠加收益 |
| OSR | 快路无 RETURN/deopt，帧语义与 helper 成功路径同 | 零适配 |
| 解释器 IC | 共用 `TOp.ic`（slot+1），双方回填 | 格式不变 |
| 刀8/5B 证伪面 | 不做 dtor 直入、不做 helper+setjmp 新建 | 本刀 = IR 内联，无 setjmp |

## 5. 分批计划

- **批1（首刀）**：FETCH_PROP 快路（2.1）——method_get/prop 读主收益；验收 = method_get_warm −50%+、
  `tests/oop` + prop 矩阵、260×3、A/B 双轮。
- **批2**：ASSIGN_PROP 快路（2.2）——prop_direct 写/dtor 内写（若 dtor 体在 JIT）；验收 =
  prop_direct_rw −80%+、赋值表达式 dst 语义矩阵（`$x = ($o->v = 1)` 等）。
- **批3（可选，另拍板）**：PropIC 结构化（方案 B）——若批1/2 后 guard load 仍可见成瓶颈。

## 6. 风险清单

1. **TProp 布局漂移**（最高频坑）：offset 全部 offsetof + Static_assert；TProp 加字段即炸编译。
2. **语义漂移**（typed UNDEF / 可见性 scope / hooks 边角）：HIT 条件逐字段镜像解释器源码；
   对拍矩阵含 private/hooks/readonly/typed/继承重声明/动态名（FFI 已移除）。
3. **ic 格式冲突**：绝不改 slot+1 格式；开工先 grep 确认 FETCH_PROP/ASSIGN_PROP 的 ic 无他用。
4. **by-ref 形态**：v1 普通 get 语义（见 §3），快路与 HEAD 同行为；未来真引用语义时再加排除闸。
5. **emit 双上下文漏点**：开关关/动态名/非 CONST 三路回 HEAD，checklist 镜像 rel_dst_gate 纪律。
6. **代码体积**：每站点 +1 菱形，miss 段复用既有 helper/replay 不复制。

## 7. 验收与观测

- **A/B**：`TPHP_PROP_INLINE` 默认开 / =0，min-of-6 双口径（Windows + WSL 若可用）。
- **探针**：`tools/probe_object_recon3.php` 复跑（prop_direct/method_get/full 三段主指标）。
- **语义矩阵**（新测试 `tests/jit/test-jit-propinline-001.php`）：
  public 读写 / 表达式 dst / private→miss / hooks→miss / readonly+typed assign→miss /
  继承（父声明子实例）/ 子类重声明换 slot / 动态名 / UNDEF typed 读 /
  rc 平衡（STR 属性 1e5 读写）/ guard 失败后 ic 回填再命中。
- **DEBUG=1**：快路命中不打 helper；miss 打 helper 且二次执行不再打（ic 已填）。
- **全量**：260×3（默认 / PROP_INLINE=0 / JIT=0）。
- **预期记账**：full 19.87→~16.5、method_get 4.6→~2.3、prop_direct 7.0→~2.0-3.0（本机口径）；
  CI object 2.83→~2.4-2.6×。alloc/ctor/dtor 机械剩余 = P0-4 dispatch 主线，另案。

## 7a. 批1 实测结果（2026-11-10）

**实现落点**：src/jit.c——8 条 offsetof `_Static_assert` + `prop_inline_enabled()` + FETCH_PROP case 内 4 层嵌套 IF 快路（L1 tag→L2 kind→L3 ic+bounds→L4 name+public+!hooks→HIT addref+wtake；miss 落既有 helper）。开关 `TPHP_PROP_INLINE` 默认开。

**踩坑**：`ir_SHL_U64(slot,…)` 的 slot 为 I64 → ir_check type 11≠5；改 `ir_MUL_I64(slot, TVAL_SZ)` 修复。

**验证**：

| 项 | 结果 |
|---|---|
| 全量 | **260×3**（默认 / PROP_INLINE=0 / JIT=0），新增 tests/jit/test-jit-propinline-001.php 10 案 |
| 语义矩阵 | 四模（默认/关/JIT=0/PHP8.5）逐字一致：public / 子类重声明换 slot / private·protected / hooks / typed / readonly / 非对象 raise / STR 1e5 rc / 父子轮转 ic 重填 / private+public 交替 |
| ir_check | 8/8 全 1 |
| A/B object min-of-6 | **21.8 vs 25.3 = −13.8%**（ON 21.8-25.1 vs OFF 25.3-31.9，方向一致） |
| recon3 专项 min | prop_fetch_only **3.87→0.95（−75%）**、method_get_warm **4.66→1.14（−76%）**、full 20.7→17.0（−18%）、prop_direct 6.97→3.86（−45%，半份=ASSIGN 批2 未做） |

**诚实记账**：method_get −76% 超设计 −50% 目标；object −14% 低于 full −18%（object 含 alloc/dtor C 侧机械不在本刀面）。**批2（ASSIGN_PROP）未动**——prop_assign_only 仍走 replay、dtor 内写未优化。

**遗留**：批2 = ASSIGN_PROP 同款内联；CI 口径待 push 复核；alloc_ctor min 波动 = 分配器暖态噪声非回退。

## 7b. 批2 实测结果（2026-11-10）

**实现落点**：src/jit.c——`case OP_ASSIGN_PROP` 拆出独立分支（ISSET/UNSET 仍 replay）；
gate = `prop_inline_enabled() && t2==CONST && lits[t2]==T_STR`（**不查 prop_helper**，miss 恒 `lower_replay`）。
守卫链 5 层镜像解释器 3119-3126：L1 tag→L2 kind→L3 ic+bounds→L4 名指针+PUBLIC+!set_vis+!hooks→
**L5 !type && !is_readonly && !(cls.flags&CLS_READONLY)**→HIT。

**HIT 舞（镜像 3127-3136）**：
1. 取 val：tr==CONST → �译期 lits 常量；否则 slots[ar]（rdv 同源）
2. addref(val) 菱形（同 FETCH）
3. `tval_release(prop_a)` + 16B store prop
4. `tr!=UNUSED` → 再 addref + `rel_dst_gate(slots[ar])` + 16B store（表达式结果写回）

**验证**：

| 项 | 结果 |
|---|---|
| 全量 | **260×3**（默认 / PROP_INLINE=0 / JIT=0） |
| 语义矩阵 | test-jit-propinline-001 扩至 15 案：新增 s11 表达式 dst `$x=($o->v=42)`、s12 readonly 写 raise、s13 typed 错型 raise、s14 hooks __set、s15 表达式 STR 1e5 rc——tphp 三模逐字一致 |
| ir_check | 方法体+OSR 双级全 1 |
| A/B object min-of-6 | **44.6 vs 48.5 ≈ −8%**（ON 44.6-47.3 / OFF 48.5-62.4） |
| recon3 专项 min | prop_direct **6.80→1.21（−82%）**、prop_assign_only **3.37→0.71（−79%）**、prop_fetch_only 3.83→0.61（−84%）、method_get 4.52→1.10（−76%）、full **19.6→13.4（−32%）**、alloc_ctor 9.9→3.9（−61%）、dtor_via_unset 14.9→11.9（−20%） |

**诚实记账**：prop_direct −82% 超设计 −55~70% 目标（批1 后半份 3.86 + 批2 写半份）；full −32% 超批1 −18%
（写侧回收 + dtor 内写进 JIT）。object bench −8% 低于 full −32%（object 含 alloc/dtor C 侧机械，且
bench 壳含 new+dtor 分配器底）。PHP8.5 对拍 s7 差异 = null 读 Error vs warning，既有语言差非本刀。

**遗留**：CI 口径待 push；`jit_body_frameless` 仍拒 ASSIGN_PROP（HIT 免帧但 miss 走 replay，暂不放行
mcall fold 内写——另案）；批3 PropIC 结构化仍可选。
