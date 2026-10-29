# backlog · 全量任务与未来规划（交接文档）

> 状态：2026-10-25 建档，2026-10-27 更新。**读者假设为零上下文的新 AI**：本文是"还剩什么活"的唯一清单——现状、全部未完成任务、远期规划、已决策不做的指针。细节规格一律点链接，不在此复述（防双账）。
> 上级索引：[../PLAN.md](../PLAN.md)。性能战略：[perf-roadmap](perf-roadmap.md)；JIT 规格：[ir-jit](ir-jit.md)。当日日志：[../memory/2026-10-27.md](../memory/2026-10-27.md)。

## 0. 现状快照（截至 2026-10-27）

- **完成**：M0–M6 + M5.5 FFI（回调 v1/v2）+ 包管理 A–E + 生成器 v1 + preg v1（P1–P4）+ C 批语法 + 解释器优化 L1–L5 + **IR baseline JIT v1.2（M1 达成，direct-slot ABI）** + **`Tphp\Thread` 真线程池**（spawn/wait/detach/poll；spawn 入口三形态 + 子线程继承全局符号表 = P1-10 已关；协程组保留为 `Tphp\TaskGroup`）。
- **测试基线**：`tests/run.ps1` = **224 passed / 0 failed**（Windows 本地与 CI 5 平台同绿）。
- **性能现状**（vs PHP 8.5.x opcache+tracing JIT 最强态）：
  - fib(28)：x86_64 ≈**1.27×**（14.0ms vs 10.9ms，direct-slot ABI 后）；
  - 未持平主体 = JIT 白名单外形态：array-read 20–51×、kv-read 15–27×、call 16×、object 7–12×；
  - aarch64 JIT **已 CI 验证启用**（P0-5：Linux fib 1.57×、macOS 1.89×，详见 memory/2026-10-27.md）。
- **目标（用户拍板）**：持平或超越原生 PHP JIT。验收 = M2 全套件几何均值 ≥1.0、M3 敏感簇反超；工具：手动 `perf.yml` workflow + 本地 `tools/perf-compare.ps1`。
- 关键事实：与 PHP 8.4+ 官方 JIT 同用 **dstogov/ir** 后端（`deps/ir/`）；spike 实测手写 fib IR 1.2ms vs Zend tracing JIT 10.9ms（9×）——**后端不是瓶颈，lowering 覆盖面才是**。

## 1. 工作协议（给新 AI 的红线）

1. **测试必须真跑**：`powershell -ExecutionPolicy Bypass -File tests/run.ps1`，224/224 才算完；禁改期望凑绿。
2. **文档同步**：动功能必须同步对应 PLAN 规格（语法→01、运行时→02、性能→perf-roadmap、JIT→ir-jit、任务账→本文）+ `memory/<date>.md` 当日日志；`memory/MEMORY.md` 完成态保持准确。
3. **构建**：Windows `cmd /c build.cmd`（结构体变更必须全量）；Unix `./build.sh`。IR 对象隔离（Windows `build/obj_ir/`、Unix `build/obj/ir_*.o`），带 main 的对象绝不进 tphp 链接。
4. **语义参考** = 本机 php 原生对拍；php-src 源码在 `C:\Users\kllxs\Desktop\php-src-php-8.5.7`。
5. **提交**：仅用户明说时提交；英文信息、单域前缀（perf/jit/feat/fix/ci/docs）。
6. **环境坑**（详见 MEMORY.md 环境坑行）：PS5.1 GBK 假乱码（用 Edit/`[IO.File]` API）；PS5.1 原生传参剥双引号（探针走临时文件）；IR builder 宏要求局部变量名 `ctx`。
7. **perf 协议**：只比纯计算（无异步/线程——PHP 没有）；bench 禁用"常量键+无副作用求和"（Zend optimizer 双态折叠，perf-roadmap §1.2）。
8. **引用计数约定（新增，2026-10-26 P1 教训）**：`tarr_append`/`tarr_set` **不做 addref**——数组接管调用方那份引用；调用方若要自己继续持有必须**先 `tval_addref`**（范例 `src/vm.c` `op_assign`）。同一对象被两处持有时两份都要独立计数，否则先释放方提前 free → UAF/二次释放。native 方法返回值默认转移所有权。
9. **内存 bug 排查（新增）**：分配器报"堆损坏"时**先建 ASAN 版本**（不定义 `TPHP_USE_MIMALLOC` → CRT malloc，ASAN 才能接管）拿分配/释放/使用三栈，别急着改 GC/分配器——2026-10-26 的 P1 前两轮（GC 哨兵、gc_tid 跨线程标记）全是误诊。流程见 `doc/testing.md`「内存问题定位（ASAN）」。
8. **JIT 调试五坑**（JIT 任务必读，详 `memory/2026-10-23.md`）：①`ir_init` 不传 `IR_OPT_CFG` ②`ir_START` 在 `PARAM` 前 ③IF 双活路必须 END+MERGE_2、沉点侧可不对称 ④**DO_FCALL 已推进 caller pc，JIT 成功路径必须 return 0** ⑤JIT→JIT 用 TLS 槽栈 trampoline，勿走帧池。

## 2. P0 · 性能主线（7× 几何均值 → M2/M3）

按预期收益排序；全部已在 perf-roadmap/ir-jit/jsc-lessons 立过项。**P0-13/P0-14 为 2026-10-28 JSC 借鉴立项**（规格 PLAN/jsc-lessons.md，用户拍板"先搬知识、以后慢慢完成"）。

| # | 任务 | 验收 | 规格/落点 |
|---|---|---|---|
| P0-1 | **JIT v1.1 白名单扩容**（**已完成**）：helper 五件套/闭包/动态调用/数组 op/循环栈已落；三形态崩溃全修（SCCP 多槽 CALL、LLP64 bitset 回绕、急切编译恢复 n_ops≤96）；**收尾（10-28）**：T_STR 字面量 + `ARRAY_APPEND` 放行（`tphp_jit_lit_set`/`tphp_jit_array_append` helper），连带修 `OP_COPY` 裸 store 双重释放（`tphp_jit_tv_copy`）。array-read 21.97→**4.22×**、kv-read 19.69→**5.84×**、throw 30.78→**3.32×**，几何均值 ~5.3→**~3.5×** | array-read 进 2–5× ✓；几何均值 ≤3× 差 ~0.5×（余量见 P0-3/P0-4） | ir-jit §12.2；src/jit.c；deps/ir；memory/2026-10-27.md、2026-10-28.md |
| P0-2 | **trampoline 免装箱 + 槽位寄存器化**（**已完成 v1.2 direct-slot ABI**）：JIT→JIT 直调（ALLOCA slots + 参数直写 + `tphp_jit_call_slots` 运行时查表），去掉 prologue guard/copy 与 TLS trampoline；MEM2SSA 评估 = 反效果（32.9ms）已弃。fib 20.8ms→**14.0ms**（vs PHP ~10.9ms ≈ 1.27×） | fib 进 1.5× 带 ✓；后续可评估 arg 直传寄存器进一步压缩 | ir-jit §12.2；src/jit.c；memory/2026-10-27.md |
| P0-3 | **字面量键 hash 缓存**（**部分完成**）：`GC_FLAG_NONNUM_KEY` 标记非数字 interned 键 → `tval_norm_key` 跳过数字扫描；`TSTR_HASH` 内联。kv-read 88.1→**84.9ms**（~4%）。剩余：opcode 级预存 hash（需改 TOp/编译器）、变量键路径深度优化 | kv 类再降 20%+ | perf-roadmap §4 表；src/value.h；src/string.c；memory/2026-10-27.md |
| P0-4 | **object / throw 路径审计**（**审计完成 + 第一轮优化 10-28**）：throw 在 Linux/aarch64 已 ~1×，Windows ~3.3×；属性 IC 已落；**本轮（拆解探针 A1-A6 定位）**：①TObj+props **单次分配**（原两次 tm_malloc——A1 29.4→17.6ms **-40%**，zend 同款 inline）②dtor 按 `m_dtor` 索引直调 `vm_call_magic_method`（免 `"__destruct"` 按名扫表）。object bench tphp 侧 72.7→**~62ms（-16%）**。**剩余定性**：解释器 dispatch 基线（A6 67ns/轮 vs PHP 16）+ 方法调用链（B 13.6 vs 4.4）+ ctor 链（A2−A1 ≈7 vs 4）→ 属 dispatch 主线（P0-11 同源）或对象进 JIT（NEW/UNSET/方法调用白名单化，大工程）；其余 magic（__get 等）按名扫表同款可修 | object 进 3× 内（dispatch 主线/对象 JIT 后再收）；throw 全平台 ≤2× | perf-roadmap §1.1；src/class.c、mem.c、vm.c；memory/2026-10-28.md |
| P0-5 | **aarch64 IR 后端接入**（**已完成，CI 验证通过**）：`ir_emit_aarch64.h` 预生成入库（dynasm），build.sh 按 TARCH 选 `-DIR_TARGET_X64/AARCH64`，`JIT_PLAT_OK=1` 全平台。CI 实测：Linux fib 1.57×、macOS 1.89× | aarch64 fib 进 x86 同带 ✓ | ir-jit §12.3；src/jit.c；deps/ir/ir_emit_aarch64.h；memory/2026-10-27.md |
| P0-6 | **packed 恢复机制**（unset 打破后重归 packed；L5 表注后置项） | 混合负载读写保持快路 | perf-roadmap §4 L2 行 |
| P0-7 | **真实负载样本**（**已完成 10-28，判据 4 落地**）：`tools/real/logstat.php`（独立真实工具：combined 日志解析+关联数组计数+top-1 遍历，CLI 直跑/bench 复用双形态，双端输出逐字节一致）+ `bench-vs-php.php` 新增 `real-logstat`/`real-jsonagg` 两项（自动进 CI perf）。**体感校准结论：real-logstat 3.87×、real-jsonagg 1.81×，13 项几何均值 ~3.19×——纯解释器真实负载与微基准同量级、聚合类反而更好，Zend optimizer 双态红利未在真实负载上放大，M3 微基准口径可信**。顺带修 parser bug（concat "Mixing" 检查误杀括号形态——`($a%60)` 也拒，真 PHP 8.5 全形态接受；paren token 豁免，无括号仍拦） | 真实体感标尺建立 ✓ | tools/real/；tools/bench-vs-php.php；src/parse.c；memory/2026-10-28.md |
| P0-8 | addref/release 热路径审计（视发现） | 出报告 | perf-roadmap §4 表 |
| P0-9 | **第二层结构性反超**（无 `&$ref` 红利兑现：逃逸分析/栈上临时值/拷贝消除；静态槽位深化；轻量协程挂起）——长期线 | 逐项立项 | perf-roadmap §4 第二层 |
| P0-10 | **带键 ASSIGN_DIM 白名单**（**已完成**）：`tphp_jit_assign_dim` 统一 append/keyed 两形态（镜像 do_dim_write 检查序），emit deopt 分支。**连带三修**：①raise 跨 IR 崩溃实证修复——`tphp_raise_guarded`（setjmp 守卫，gdb/cdb 定位真凶之一是 helper 内 `tphp_vm_raise` 多传 NULL 当 fmt→vsnprintf(NULL) 崩）②deopt 传播链闭环（helper→IR RETURN 0→call_slots/dyn 逐层→op_do_fcall C 帧 longjmp，try/catch 捕获实测✓）③**热路径 setjmp 快慢路分离**（setjmp+2×jmp_buf ≈20ns/call 使 array-push +24ms，成功路径归零）。array-rmw 12.9→**5.7ms（6.79→3.0×）**，几何均值 ~3.5→**~3.29×** | array-rmw 进 5× 带 ✓（3.0×） | src/jit.c eligibility+helper；memory/2026-10-28.md |
| P0-11 | **macOS x64 "病态"排查**（**已结案 10-28：非 bug**）：perf.yml `macos-x86_64` 本就是 **ARM64 runner 交叉编译 → Rosetta 2 转译运行 + PHP 侧 arm64 原生**的不对等对比（workflow 注释原文 report-only）——解释器 threaded dispatch 是 Rosetta 转译弱项，精确解释"fib 正常（简单循环转译质量高）、解释器 10–50×"指纹。**防再误读落地**：perf-compare.ps1 报告头加 **arch 对等检查**（php_uname vs tphp -v 架构不一致 → 大字 WARNING 不可比）+ perf.yml 两 macOS job 加环境转储（uname/CPU/`sysctl.proc_translated`）+ job 注释显式标注。**遗留小尾巴**：arm64-mac（原生）object 211ms vs linux-arm 68ms 的 3×——疑 runner CPU 代差，下轮 CI 环境转储数据到后判定 | x64-mac 数据自解释 ✓；arm64-mac 3× 待 CPU 数据 | tools/perf-compare.ps1；.github/workflows/perf.yml；memory/2026-10-28.md CI 节 |
| P0-12 | **残留跨 IR raise 修复**（**已完成**）：`fetch_dim_r`（raise 全走 `raise_guarded` 冷守卫，成功路径零 setjmp，补 **string offset 读语义**——原版跨 IR 且错报 TypeError，解释器 $s[0] 返回字符）+ `binop_slow`/`cmp_slow`（tval_* 内部 raise 必须事前 setjmp，bcall 同构 wrapper，成本仅慢路径）+ emit 全 6 调用点 deopt 分支（call_slow 封装内加，调用方零改动）。四场景 JIT/解释器输出一致（Undefined array key、Uninitialized string offset、Unsupported operand types、负索引 $s[-1]=c） | 残留跨 IR raise 清零（余 closure_init 内部错误/dyn fallback/FFI base 三项边界记录在案）；224/224 绿；perf 同带 | src/jit.c；memory/2026-10-28.md |
| P0-13 | **per-site 单态 IC**（**v1+v2 已落地 10-29，未达 2× 验收**）：v1 = `DimIC` 32B 快照挂 `TOp.ic` + 解释器/JIT helper 双路接入（kv -14%，P0-3 并入）。**v2（10-29）= 命中路径全内联**：每 FETCH_DIM_R 生成 packed 直取（`(u64)key<(u64)n` 单条 unsigned 比较）+ DimIC hash 链 7 层（链序照抄 tarr_get_ic，buckets 比较先于 bp 解引用=撕裂安全门）双快路，命中 leaf `tphp_jit_dim_take`，miss/异常全落 v1 helper+deopt（语义零变化）；**generic 回退门**（`DimIC.misses` 链失败 ++、仅叶子命中清零、helper 回填不清；≥8 跳链直 call 永久锁死——JSC generic 同款）。**排障**：`ir_ZEXT_U32` 产出仍 U32 → ir_SHL_U64 类型不兼容且 **ir_check=0 不 gate 照常出码**（铁律：每轮编译盯 ir_check=1）。**实测 array-read 13.4→10.9ms（-19%，2.79×）、轮转键 kv 锁死门止血 63.6→56.6（5.39×）、几何均值 3.19→~3.04×，224/224 绿**。**v3+v3.1（10-29 同日续）**：多态 N-case 链（`DimIC`=共享 arr/bk+`cases[8]` FIFO+`dead` 哨兵——单 bool 归一防两入口递归膨胀；写序铁律 cases 先于共享层）+ take 全内联（镜像 addref/release 热路径：先 addref 后 release 防自指归零、rc>1 与 DEAD 正交免查 color、慢路 call 原样；C 版 helper 删）。**当日合计：array-read 15.6→9.9ms（-26%，2.68×）、kv 52.7（多态全内联与 v1 锁死打平）、几何均值 3.19→~3.06×，224/224、每轮 ir_check=1** | **未进 2×，余量候选**：case 链顺序扫优化（move-to-front/桶 hash 直达）、kv 差距已分散需 profile 归因，或转 **P0-14 profile 准入**（几何均值主杠杆） | PLAN/jsc-lessons.md §2；src/value.h、src/array.c、src/vm.c、src/jit.c；memory/2026-10-29.md |
| P0-14 | **profile 驱动 JIT 准入**（**立项 10-28；10-29 第一批 = 拒因审计先行**）：①**审计基建落地**——eligibility 13 处拒因收口 + `reject(名): 拒因` 日志（TPHP_JIT_DEBUG）；分布 = CONCAT×5 / DIV-MOD×3 / FE_RESET×1 / INIT_NEW×1。②**DIV/MOD 放行 ✓**（trace 实证编译器物化字面量 → t2 恒槽，旧 const 检查过严；emit 本就支持槽、零除走 slow+deopt）。③**CONCAT helper/emit 就绪但暂缓**——LOOP 内 ≥3-4 连 CONCAT CALL 触发 deps/ir codegen 执行错（IR 参数层全对+ir_check=1、第 N 个 call 的 slot 地址参数执行期变野值；s2=4连无MOD崩/b4=2连过/MOD 独立过；O2 multi-slot CALL 同族嫌疑，ir_bisect 复现工具在）→ eligibility 撤回（helper/emit 保留，复现式见 memory），**待上游排查重放行**。④profile 观察计数机制本体（jsc-lessons §3）未动——审计结论其杠杆排在 CONCAT 修复后 | ①余量：CONCAT 重放行（backend 排查）②FE_RESET/INIT_NEW 归批2/3③profile 机制 | PLAN/jsc-lessons.md §3；src/jit.c；memory/2026-10-29.md |

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
| P1-9 | **属性类型 FQCN 解析缺口**（实测 2026-10-27）：public \A \; 报 Parse error；PHP 允许。pebview 已用 docblock 绕开。落点 parse.c 属性类型 | 01 §2 类型声明 |
| P1-10 | **Thread.spawn 库内函数支持**（**已完成 2026-10-27**）：入口三形态 Closure / string 全局函数名（main+inc_progs+builtins，`tphp_vm_resolve_fn_global`）/ `[Cls,'method']` 静态方法（`class_resolve_lc` 走 autoload）；子线程经 `tphp_vm_run_closure(prog, share, …)` 继承父线程 Program 快照（跨文件调用与父同表）；入口闭包捕获预检 `worker_cap_ok`（FFI CData/无 `__cloneState` → NotCloneableError 指引改三形态/static function）；`worker_unserialize_closure` 补 `clo->scope=NULL`（P1-3 漏改，tm_malloc 不清零）。pebview bindAsync spawn 闭包改 `static function`（方法内普通闭包自动捕获 $this）。测试 `tests/worker/test-thread-scope.php` 8 例 | 02 §Thread；pebview src/Window.php | GUI 端到端（index.php 慢 Sum）待手动跑；FFI cast 已放开（2b56c5f） |
| P1-11 | **P0-7 实测暴露的 compat 三缺口**：①`json_decode` 默认 assoc——tphp 默认 true 且**无 stdClass 基建**（注释误称"PHP7+默认 array"，PHP 实为 stdClass/`json_decode($j,true)` 才是数组；修需 stdClass 动态属性对象）②**arsort/asort/ksort 排序族缺失**（有 sort/rsort/usort，真实代码高频——logstat 被迫手写 top 遍历）③**concat "Mixing" 禁令比 PHP 严**（真 PHP 8.5 全形态接受 `$a+$b."c"`；根因 tphp `.` 优先级低于 `+` 与 PHP 同级左结合不同——正解是对齐 PHP 优先级后删禁令，paren 豁免已先行落地 parse.c） | 真实代码不再遇三缺口 | ext/json/json.c；ext/builtin；src/parse.c concat 层级；memory/2026-10-28.md |
| P1-12 | **循环 OSR**（**立项 10-28**，大工程规格先行）：JSC 模式（dfg/DFGOSREntry.cpp 416 行 / DFGOSRExit.cpp 921 行 + availability analysis）——解释器热循环进 JIT 代码、投机失败逐 slot 恢复回退；函数级准入抓不住"循环体含白名单外 op"的函数，敏感簇 call/read/kv 与 A6 dispatch 67ns 大量发生在循环内。**M3 敏感簇反超主力组合**（循环 OSR + P0-13 IC + P0-14 profile 准入）。规格前提：循环体 op 子集、VMFrame→JIT slots 映射、loop header entry 检查成本上限、exit 与异常/协程交互（复用 P0-12 框架） | M3 敏感簇反超 | PLAN/jsc-lessons.md §4；关联 P0-9、P0-11 |

## 4. P2 · 工程线

| # | 任务 | 记录 |
|---|---|---|
| P2-1 | pkg lock **E3**：zip 源字节哈希、树哈希压缩、`tphp verify` 独立命令 | pkg-manager §E3 |
| P2-2 | pkg `autoload.classmap`；pack.include/exclude 的 build 消费 | pkg-manager 边界表、memory 余项 |
| P2-3 | **pkg 全局缓存**（版本寻址 + 原子 rename + 硬链接，Bun 模式）与 keepalive 连接池 | perf-roadmap §6 表 |
| P2-4 | pkg 注册表（`upload` 指引后的远期项） | pkg-manager §upload |
| P2-5 | FFI 回调残留：~~可捕获异常~~ **v2-3 已完成（2026-10-27）**；余 struct 字段/数组元素闭包赋值 v2 核对、trampoline 去重复核、延迟异常传播到调用点外（**存疑候选**：与 PHP 语义取舍，默认不做） | 02 §4.5.1 |
| P2-6 | **FFI DLL 打包器**（可选后置） | 04 §M5.5 行、03 §6.1 |
| P2-7 | php-compat 对拍层加厚（现仅 17 例，从 Zend/tests 扩到几十例） | memory 缺口清单 |
| P2-8 | standard 扩展测试加厚（现仅 1 个聚合文件） | memory 缺口清单 |
| P2-9 | CI 加 **ASAN/valgrind job**（doc/build.md 仅手工开启） | memory 缺口清单 |
| P2-10 | doc/ffi.md 信任声明（"装包即信任"一句话） | memory 余项 |
| P2-11 | `date()` 时区差异拍板：tphp 固定 UTC vs PHP 本地时区——记入 01 §10 还是改实现 | memory/2026-10-23.md L84 |
| P2-12 | 01 §10 内部矛盾拍板：`&get` 一行说"后置"、§12.3 说"不做（与 &$ref 冲突）"——统一权威表述 | 01 §10 vs §12.3 |
| P2-13 | **JIT 编译异步化**（后台线程编译，ir-jit §12.3）——2026-10-28 补 JSC 参照：`jit/JITWorklist.cpp`（344 行）任务队列+后台工作线程+完成回调 arm，当前帧继续解释；约束=编译线程不得触碰 VM 堆，候选形态=仅读 freeze 后只读 Program，产物原子挂回 | ir-jit §12.3；jsc-lessons §5 |
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

- 全量测试：`powershell -ExecutionPolicy Bypass -File tests/run.ps1`（期望 222/0）。
- 性能对比：本地 `pwsh tools/perf-compare.ps1 -MaxFibRatio 6.0 -Report perf.md`；CI 手动 Actions → **Perf** → Run workflow（`fib_gate` 输入可调，0=仅报告）。
- 单 bench：`build/tphp.exe run tests/bench/test-bench-*.php`（计时走 stderr）。
- JIT 开关 `TPHP_JIT=0`；调试 `TPHP_JIT_DEBUG=1`；执行 trace `TPHP_TRACE=1`；opcode 查看 `--dump-opcodes`。
- **内存 bug**：ASAN 版本（`TPHP_USE_MIMALLOC` 关 + `-fsanitize=address`）——流程见 `doc/testing.md`「内存问题定位（ASAN）」。
- 历史决策检索：`memory/MEMORY.md`（锚点）→ `memory/<date>.md`（当日细节）→ 各 PLAN（权威规格）。
