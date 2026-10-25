# backlog · 全量任务与未来规划（交接文档）

> 状态：2026-10-25 建档。**读者假设为零上下文的新 AI**：本文是"还剩什么活"的唯一清单——现状、全部未完成任务、远期规划、已决策不做的指针。细节规格一律点链接，不在此复述（防双账）。
> 上级索引：[../PLAN.md](../PLAN.md)。性能战略：[perf-roadmap](perf-roadmap.md)；JIT 规格：[ir-jit](ir-jit.md)。

## 0. 现状快照（截至 2026-10-25）

- **完成**：M0–M6 + M5.5 FFI（回调 v1/v2）+ 包管理 A–E + 生成器 v1 + preg v1（P1–P4）+ C 批语法 + 解释器优化 L1–L5 + **IR baseline JIT v1（M1 达成）**。
- **测试基线**：`tests/run.ps1` = **218 passed / 0 failed**（Windows 本地与 CI 5 平台同绿）。
- **性能现状**（vs PHP 8.5.x opcache+tracing JIT 最强态，首次全平台 CI 基线见 `memory/2026-10-23.md`）：
  - fib(28)：x86_64 0.4×–2.7×（tphp JIT 已覆盖）；全套件几何均值 **≈7×（Linux CI）**；
  - 未持平主体 = JIT 白名单外形态：array-read 20–51×、kv-read 15–27×、call 16×、object 7–12×；
  - aarch64 平台 tphp JIT 未启用（v1 = x86_64 only）。
- **目标（用户拍板）**：持平或超越原生 PHP JIT。验收 = M2 全套件几何均值 ≥1.0、M3 敏感簇反超；工具：手动 `perf.yml` workflow + 本地 `tools/perf-compare.ps1`。
- 关键事实：与 PHP 8.4+ 官方 JIT 同用 **dstogov/ir** 后端（`deps/ir/`）；spike 实测手写 fib IR 1.2ms vs Zend tracing JIT 10.9ms（9×）——**后端不是瓶颈，lowering 覆盖面才是**。

## 1. 工作协议（给新 AI 的红线）

1. **测试必须真跑**：`powershell -ExecutionPolicy Bypass -File tests/run.ps1`，218/218 才算完；禁改期望凑绿。
2. **文档同步**：动功能必须同步对应 PLAN 规格（语法→01、运行时→02、性能→perf-roadmap、JIT→ir-jit、任务账→本文）+ `memory/<date>.md` 当日日志；`memory/MEMORY.md` 完成态保持准确。
3. **构建**：Windows `cmd /c build.cmd`（结构体变更必须全量）；Unix `./build.sh`。IR 对象隔离（Windows `build/obj_ir/`、Unix `build/obj/ir_*.o`），带 main 的对象绝不进 tphp 链接。
4. **语义参考** = 本机 php 原生对拍；php-src 源码在 `C:\Users\kllxs\Desktop\php-src-php-8.5.7`。
5. **提交**：仅用户明说时提交；英文信息、单域前缀（perf/jit/feat/fix/ci/docs）。
6. **环境坑**（详见 MEMORY.md 环境坑行）：PS5.1 GBK 假乱码（用 Edit/`[IO.File]` API）；PS5.1 原生传参剥双引号（探针走临时文件）；IR builder 宏要求局部变量名 `ctx`。
7. **perf 协议**：只比纯计算（无异步/线程——PHP 没有）；bench 禁用"常量键+无副作用求和"（Zend optimizer 双态折叠，perf-roadmap §1.2）。
8. **JIT 调试五坑**（JIT 任务必读，详 `memory/2026-10-23.md`）：①`ir_init` 不传 `IR_OPT_CFG` ②`ir_START` 在 `PARAM` 前 ③IF 双活路必须 END+MERGE_2、沉点侧可不对称 ④**DO_FCALL 已推进 caller pc，JIT 成功路径必须 return 0** ⑤JIT→JIT 用 TLS 槽栈 trampoline，勿走帧池。

## 2. P0 · 性能主线（7× 几何均值 → M2/M3）

按预期收益排序；全部已在 perf-roadmap/ir-jit 立过项，无新账。

| # | 任务 | 验收 | 规格/落点 |
|---|---|---|---|
| P0-1 | **JIT v1.1 白名单扩容**：闭包调用域化（CLOSURE_INIT/SEND_CAP，call bench）、`FETCH_DIM_R` packed 快路（array-read/kv）、POW/变量移位 slow helper、float 参数 prologue、for/while 循环全形态验证 | array-read/kv/call 进 2–5× 带；几何均值先 ≤3× | ir-jit §12.2；`src/jit.c` |
| P0-2 | **trampoline 免装箱 + 槽位寄存器化**（arg 直传寄存器；评估 `IR_OPT_MEM2SSA`） | fib 2.7× → ~1× | ir-jit §12.2；`src/jit.c` |
| P0-3 | **字面量键 hash 缓存**（opcode 预存字面量键 hash，kv 类 2.3× 形态差；JIT 外路径同受益） | kv 显著下降 | perf-roadmap §4 表 |
| P0-4 | **object / throw 路径审计**（vs JIT 7–12× / 4×）→ 解释器优化或进白名单 | 出审计结论并落地至少一半差距 | perf-roadmap §1.1；对象属性单态 guard 见 ir-jit §12.3 |
| P0-5 | **aarch64 IR 后端接入**（IR 自带 AArch64；放开 `JIT_PLAT_OK`） | aarch64 fib 10–14× → 进 x86 同带 | ir-jit §12.3 |
| P0-6 | **packed 恢复机制**（unset 打破后重归 packed；L5 表注后置项） | 混合负载读写保持快路 | perf-roadmap §4 L2 行 |
| P0-7 | **真实负载样本**（tphp 重写的小工具当体感标尺——roadmap 判据 4 一直未做） | 1–2 个真实脚本进 bench | perf-roadmap §3 |
| P0-8 | addref/release 热路径审计（视发现） | 出报告 | perf-roadmap §4 表 |
| P0-9 | **第二层结构性反超**（无 `&$ref` 红利兑现：逃逸分析/栈上临时值/拷贝消除；静态槽位深化；轻量协程挂起）——长期线 | 逐项立项 | perf-roadmap §4 第二层 |

**里程碑判据**：M2 = perf.yml 几何均值 ≥1.0；M3 = 敏感簇（fib/call/read/push/kv/rmw/object）全部 ≤1.0×。aarch64 门槛在 P0-5 后从"仅报告"改为硬门。

## 3. P1 · 功能线（立项类先写规格再动手）

| # | 任务 | 规格/记录 | 备注 |
|---|---|---|---|
| P1-1 | 生成器 v2：`throw()`、`yield from`、dtor-跑-finally、一般 Iterator/Traversable foreach | generators.md §v2 表 | 独立批 |
| P1-2 | **M7 大项四候选**（各自先出 PLAN 规格）：①HTTP server（零拷贝设计 perf-roadmap §6）②PDO/内置 SQLite（libsqlite3 vendor、协程化）③REPL ④`#[Attr]` 注解 + Reflection 最小集 | 02 §后置、01 §12.2-B | 01 明示注解"等需求出现再立项防空转" |
| P1-3 | 结构化并发：`Tphp\scope()` / `select()` / `withTimeout($c,$ms)` | 02 §M7+ | 规格先行 |
| P1-4 | preg 后置批：`preg_replace_callback`、数组形态 replace、**PCRE2 JIT 启用**、INI 面（backtrack_limit 等） | preg.md 状态行 | 低成本 |
| P1-5 | `yield from` 若不在 P1-1 内单独做 | generators.md | 与 P1-1 合并即可 |
| P1-6 | WeakMap / WeakReference | 01 §不支持清单 | 立项时改 01 |
| P1-7 | **FFI Unix 补齐核实**：02 记载 dlopen（Linux/macOS）后置，但 unix CI ffi 测试已绿——确认实际状态并同步 02 文档；缺则补 | 02 §FFI 实现行 | 文档一致性任务 |
| P1-8 | 动态静态方法名 + 命名参数 `C::$v(a:1)`（现为加载期拒绝，01 §10 标注的唯一未实现项） | 01 §10 | 小项 |

## 4. P2 · 工程线

| # | 任务 | 记录 |
|---|---|---|
| P2-1 | pkg lock **E3**：zip 源字节哈希、树哈希压缩、`tphp verify` 独立命令 | pkg-manager §E3 |
| P2-2 | pkg `autoload.classmap`；pack.include/exclude 的 build 消费 | pkg-manager 边界表、memory 余项 |
| P2-3 | **pkg 全局缓存**（版本寻址 + 原子 rename + 硬链接，Bun 模式）与 keepalive 连接池 | perf-roadmap §6 表 |
| P2-4 | pkg 注册表（`upload` 指引后的远期项） | pkg-manager §upload |
| P2-5 | FFI 回调残留：struct 字段/数组元素闭包赋值 v2 余项核对、trampoline 去重复核、延迟异常传播（**存疑候选**：与硬 Fatal 是语义取舍，默认不做） | 02 §4.5.1 |
| P2-6 | **FFI DLL 打包器**（可选后置） | 04 §M5.5 行、03 §6.1 |
| P2-7 | php-compat 对拍层加厚（现仅 17 例，从 Zend/tests 扩到几十例） | memory 缺口清单 |
| P2-8 | standard 扩展测试加厚（现仅 1 个聚合文件） | memory 缺口清单 |
| P2-9 | CI 加 **ASAN/valgrind job**（doc/build.md 仅手工开启） | memory 缺口清单 |
| P2-10 | doc/ffi.md 信任声明（"装包即信任"一句话） | memory 余项 |
| P2-11 | `date()` 时区差异拍板：tphp 固定 UTC vs PHP 本地时区——记入 01 §10 还是改实现 | memory/2026-10-23.md L84 |
| P2-12 | 01 §10 内部矛盾拍板：`&get` 一行说"后置"、§12.3 说"不做（与 &$ref 冲突）"——统一权威表述 | 01 §10 vs §12.3 |
| P2-13 | **JIT 编译异步化**（后台线程编译，ir-jit §12.3） | ir-jit §12.3 |
| P2-14 | Transpiler 磁盘缓存（键=内容哈希；需 AST 可序列化改造——记账不急） | perf-roadmap §6 表 |
| P2-15 | **Profiler 立项**（做时学 Bun 分级 trace 埋点式；现有 TPHP_TRACE 逐指令太重） | perf-roadmap §6 表 |
| P2-16 | L4 VM 全宏化（computed-goto threaded interpreter）——**独立立项候选**，非近期，防翻案记录在案 | perf-roadmap §4 L4 行 |

## 5. 远期里程碑（判据，不是排期）

1. **M2**：perf.yml 几何均值 ≥1.0（vs PHP opcache+tracing JIT 最强态）。
2. **M3**：敏感簇逐项 ≤1.0×（结构性反超兑现，P0-9 是主力）。
3. **M7**：P1-2 四候选落地（HTTP server / SQLite / REPL / 注解），配合真实负载样本（P0-7）形成"能写真实应用"叙事。
4. **全平台一致**：P0-5 后 aarch64 收硬门；perf.yml 成为每轮性能回归的固定闸门。

## 6. 已决策不做（防翻案——改主意前先读原文）

- 01 §12.3 与 §9：`&$ref`/`extract`/`eval`/`@`/错误处理器（warning→异常）、`${var}` 插值、`each()`、`declare(ticks/encoding)`、嵌套条件函数声明、引用捕获/引用传参、resource 类型。
- perf-roadmap §8：借壳 JSC/V8、PHP→JS/WASM 转译、用纯解释器追平 JIT、io_uring 抢先、为性能牺牲语义。
- ir-jit §13：体内异常路径 JIT、v1 即做数组/字符串、自研 regalloc、与解释器分两套语义。
- 其他：FFI `ffi.enable` 总闸、动态库标识/签名（ffi-security 拍板）；pc 类 `pack.include`… 见 pkg 边界表。

## 7. 验证与工具速查

- 全量测试：`powershell -ExecutionPolicy Bypass -File tests/run.ps1`（期望 218/0）。
- 性能对比：本地 `pwsh tools/perf-compare.ps1 -MaxFibRatio 6.0 -Report perf.md`；CI 手动 Actions → **Perf** → Run workflow（`fib_gate` 输入可调，0=仅报告）。
- 单 bench：`build/tphp.exe run tests/bench/test-bench-*.php`（计时走 stderr）。
- JIT 开关 `TPHP_JIT=0`；调试 `TPHP_JIT_DEBUG=1`；执行 trace `TPHP_TRACE=1`；opcode 查看 `--dump-opcodes`。
- 历史决策检索：`memory/MEMORY.md`（锚点）→ `memory/<date>.md`（当日细节）→ 各 PLAN（权威规格）。
