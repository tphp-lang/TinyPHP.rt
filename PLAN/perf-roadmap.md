# perf-roadmap · 性能路线（解释器 → IR JIT）

> 状态：战略定稿 2026-10-23。上级索引：[../PLAN.md](../PLAN.md)。本文档是性能方向的权威记录——参照系、时机判据、优化清单、JIT 路线与防翻案结论。执行层面的 bench 协议见 [../doc/testing.md](../doc/testing.md)。

## 1. 现状与差距来源

当前 tphp 是纯解释器（自有 opcode + VM，无 JIT），单线程纯计算慢于 php-src 8.5（Zend VM + 可选 opcache/JIT）。差距来源按权重：

1. **JIT**：PHP 8 可开 Tracing JIT（生产常开）——数值/热点代码可数倍于解释器
2. **25 年 VM 打磨**：opcode 特化 handler、INIT_FCALL inline cache、interned strings、packed arrays、不可变数组
3. **opcache 字节码缓存**：省重复编译

tphp 的**结构性优势**（PHP 因包袱做不到）：无 `&$ref`（纯值语义 TVal，无 IS_REFERENCE 别名棱角）、无 symbol_table 历史、帧结构简单、单二进制无 SAPI/opcache 固定开销、无色协程（PHP 无等价物）。

## 2. 参照系（三层，防混淆）

| 层 | 参照物 | 目标 |
|---|---|---|
| 纯 VM 吞吐 | **Zend（opcache 开、JIT 关）** | 追平 0.8–1.2x（第一层优化）→ 结构性反超 1.2–2x（第二层） |
| 运行时工程层 | **Bun**（安装/IO/bench 方法论，非引擎） | 见 §6 |
| 超越"开 JIT 的 PHP" | **IR 自研字节码 JIT**（§5）+ 姊妹 AOT 分工 | 远期主路径，时机见 §3 |

**排除性结论（防翻案）**：
- ❌ 借壳 JSC/JSC JIT——JIT 不可剥离语言引擎（JSC 编译 JS 字节码，与 JSValue/JSCell 对象模型耦合）；"PHP→JS 转译"是 HipHop for PHP 用十年验证过的死路（语义保真灾难 + Facebook 官方放弃 PHP 兼容）
- ❌ WASM/嵌 HHVM——同理或已不兼容 PHP
- ✅ 姊妹项目 TinyPHP（AOT）是数值密集场景的既定答案；tphp 热点可用 FFI 调 C/TinyPHP 产物（现成逃逸舱）

**关键事实**：绝大多数真实 PHP 部署**不开 JIT**（共享主机/默认 ini）。追平"开 opcache 的 PHP"即超过约 90% 的实际部署，且 tphp 额外附送无感异步 + 真线程 + 单二进制——性能叙事主轴在此，不与 Zend JIT 正面决战（决战路径见 §5，已因 IR 大幅拉近）。

## 3. 优化时机：四判据（满足前不动手）

1. **语义/语法到达"能写真实应用"阈值**：C 小语法包（[01-syntax-spec](01-syntax-spec.md) §12.1）+ 生成器（§12.2-A）+ `preg_*`（热路径刚需，无它 benchmark 不代表真实负载）
2. **性能语义冻结**：PLAN/01 差异清单稳定、php-compat 对拍层足够厚（优化常伴随语义微调，未稳即返工）
3. **bench 套件 ≥10 项且进 CI**（现在仅 3 项微基准）：调用/字符串/数组/异常/协程/序列化/JSON 分类齐全，有 CI 回归记录
4. **有真实负载样本**：用 tphp 重写的真实工具执行时间当体感标尺

其中判据 3（bench 扩充）**现在即可做**，是唯一不被后续功能作废的前置投资。

## 4. 解释器内优化（IR JIT 之前的第一二层）

### 第一层：追平无 JIT PHP（按预期收益排序）

| 项 | 嫌疑点/机会 | 预期 | 状态 |
|---|---|---|---|
| **帧池化** | 每函数调用 `tm_malloc` VMFrame——调用密集负载最大嫌疑点；freelist 池化 | 调用型 10–20% | ✅ **2026-10-24 完成：call 298→175ms（-41%）、fib 198→114（-42%）**；VMFrame 头 + slots 分档 4..256 双 freelist（上限 256）；sched 协程根帧同池 |
| Packed array 快路径 | TArr 统一桶哈希，连续 int 键每次哈希；PHP packed arrays 是其主要优化之一 | 数值循环显著 | ✅ **L2 读路径完成：arr-read 76→64-77ms（-10~15%）**；TArr.packed 标志（纯追加无洞保持），tarr_get int 键直取；写路径/packed 恢复后置 |
| Interned string 强化 | 热路径 `===`/键查找确保走 pointer 比较分支 | 字符串密集 5–15% | ✅ **L3 完成但收益低于预期：数组字符串键 intern 后 kv-read 仅 -3%**（tstr_eq 的 hash 短路已覆盖大半 memcmp）；`===` 已走 tstr_eq 最优路径。保留 3% 免费收益；新增 kv-read bench 固化基线 |
| dispatch 特化 | clang computed-goto threaded interpreter、热点 opcode 排布 | 全局 5–10% | ⚠️ **L4 评估收口（2026-10-24）**：当前架构 = 每 opcode 经 `TOp.handler` 函数指针间接调用；computed-goto 要求 handler 内联为循环内标签 = **zend_vm_def.h 式全宏化重构**（1.1 万行先例，数周级、高风险），非"排布"级轻量项。DO_FCALL 热路径审计：特例分支在前但主体已紧凑（帧池化后 call 175ns 中 dispatch 占比 <15%）。**决策：不强行做**——L1–L3 已取 call -41%/fib -42%/arr-read -15%；本项转为"独立立项候选"（全宏化重构）或接受现状进第二层 |
| addref/release 审计 | 热路径临时 TVal 是否过度引用计数 | 视发现 | 未开始 |
| **string .= in-place** | 2026-10-24 对比 PHP 发现 **325× 单点灾难**（292 vs 0.9ms）：tstr_concat 不可变全量重分配 O(n²) vs zend rc=1 原地指数扩容 | 消灭最大单点 | **新增，下一优先** |
| **array packed 写路径** | 对比 push **15×**（297 vs 20ms）：append 走全套 tarr_set；PHP packed append 直接 data[n]++ | 15×→3–5× | **新增，次优先**（L2 后置项有了数据） |

### 第二层：结构性反超（PHP 做不了的）

- 无 `&$ref` 红利：逃逸分析、栈上临时值、拷贝消除（Zend 因引用可逃逸全部锁死）
- 全静态槽位化的自由变量/全局访问继续深化
- 协程挂起路径比 zend_fiber 更轻（帧结构简单）——"附赠并发还比你快"的支点
- 单二进制固定开销差

## 5. JIT 路线：IR 框架（2026-10-23 重大发现）

### 5.1 IR 的身份（为什么这条路成立）

- **PHP 8.4+ 官方 JIT 的底座**（README 原文 "used as a base for PHP JIT since PHP-8.4"）；作者 Dmitry Stogov = Zend 首席开发者、Zend JIT 作者本人；版权 "Zend by Perforce"；MIT
- **官方集成方式 = vendor 源码嵌入**（php-src 放 `ext/opcache/jit/`，README 明示推荐）——与 tphp `deps/` 惯例同构
- 能力：Sea-of-Nodes IR（HotSpot/V8 TurboFan 同族）；fold→SCCP→GCM→调度→指令选择→Linear Scan regalloc；后端 x86_64/x86/AArch64（DynASM）+ C/LLVM 输出；GDB JIT interface、perf、graphviz 工具链
- 本机源码：`C:\Users\kllxs\Desktop\ir-master`（研究用；vendor 时锁与 php-src 8.5.7 对齐的 commit）

### 5.2 对接草案（渐进 baseline JIT）

```
解释器执行 → 函数热点计数 → 阈值命中：
  该函数 opcode → IR lowering（带 TVal 类型 guard）
  → IR 编译机器码；入口 guard：
      int/float 密集路径 → untagged 寄存器运算
      guard 失败（对象/字符串/异常/FFI/协程）→ deopt 回解释器
```

- 分工：lowering + 类型 guard/deopt = tphp；优化/regalloc/机器码 = IR 全包
- **公平竞赛点**：Zend JIT 8.4+ 与 tphp 用同一后端——胜负取决于 lowering 质量；tphp 纯值 TVal 无 IS_REFERENCE 棱角，guard 更干净（结构性优势）
- 对照设计：本机 php-src 8.5.7 的 `ext/opcache/jit/zend_jit.c`（zval lowering 直接可抄）
- v1 范围：函数级 baseline，**不做 OSR**（只 JIT 从入口进的函数，砍一半复杂度）；不碰异常路径/FFI/协程/对象密集代码
- 首发平台：Windows x64（IR 支持 ✓）

### 5.3 风险

- IR 自述 "not a stable finished product"——PHP 8.4/8.5 生产在用；对策 = vendor 锁版本，升级跟随 php-src
- DynASM 构建步骤进 build 脚本（fiber 汇编同款先例）
- deopt 是 JIT 最难两块之一（v1 靠"入口 guard + 失败弃用 JIT 回解释器"简化）

### 5.4 时机

§3 判据满足后立 JIT 专项（先出 opcode→IR lowering 规格）；可选前置 spike（半天）：vendor `deps/ir/` + 跑通自带 bench + 手写 fib 打样，消除构建风险。

## 6. Bun 可借鉴清单（引擎外工程层）

> Bun 快的底座是嵌入 JSC（自研解释器无法复制）；以下是引擎外、与解释器无关、可直接抄思路的部分。

| 项 | Bun 机制 | tphp 落点 | 优先级 |
|---|---|---|---|
| pkg 全局缓存 | 版本寻址缓存 + tarball 解临时目录原子 rename + node_modules 硬链接/clonefile + keepalive 连接池 | `~/.tphp/cache` 版本寻址 + install/add 二次近零拷贝；与 integrity 快照配合（缓存命中也过哈希） | pkg 下一阶段 |
| IO 零拷贝降级链 | copy_file_range→sendfile→splice→read/write；HTTP 静态文件 sendfile；uWS cork 合并写 | M7 HTTP server 立项时进规格；IO 层逐步接入 | M7 时 |
| io_uring | **Bun 也没用**（仅 TODO）——业界靠线程卸载 | tphp libuv 线程池路线够用，不焦虑 | — |
| Transpiler 磁盘缓存 | 键 = 内容哈希+特性哈希+格式版本 | tphp 的 TOp 含 handler 指针**不可序列化**（bundle 改源码内嵌的同一原因）；缓存需 AST/字节码可序列化改造——记账不急做 | 后置 |
| Benchmark 方法论 | 真实负载（oha/真实应用）+ 微基准并列；多运行时同脚本；反 ab/autocannon | bench 扩充（§3 判据 3）按此约定 | **现在** |
| 分级 trace | BUN_TRACE 编译期常量埋点，禁用零开销 + tracy | tphp 的 TPHP_TRACE 是逐指令级（太重）；做 profiler 时学埋点式 | profiler 时 |

## 7. 行动顺序（汇总）

1. **现在**：bench 套件扩充 + CI（§3 判据 3；方法论按 §6 Bun 约定）——可与功能批次并行
2. **功能阈值**：C 小语法包 → 生成器 → `preg_*`（[01 §12](01-syntax-spec.md)）
3. **解释器优化窗口**：§4 第一层（帧池化/packed array 起步）
4. **JIT 立项**：spike（vendor IR）→ opcode→IR lowering 规格 → baseline JIT 实现
5. **随 M7**：HTTP server 按 §6 零拷贝设计立项；pkg 缓存按 Bun 模式立项

## 8. 不做清单（防翻案）

| 项 | 原因 |
|---|---|
| 借壳 JSC/V8 等现成 JS 引擎 JIT | JIT 与语言引擎对象模型深度耦合，不可剥离 |
| PHP→JS/WASM 转译 | HipHop 十年验证的死路（语义保真灾难） |
| 追平"开 JIT 的 Zend"作为近期目标 | 远期走 IR 自研 JIT（§5），近期目标是无 JIT Zend |
| io_uring 抢先接入 | 业界（含 Bun）未解决通用异步文件 IO，线程卸载够用 |
| 为性能提前牺牲语义兼容 | 对拍层是护城河；优化在语义冻结后 |
