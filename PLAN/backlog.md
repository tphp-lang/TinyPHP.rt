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
| P0-14 | **profile 驱动 JIT 准入**（**立项 10-28；10-29 第一批 = 拒因审计先行**）：①**审计基建落地**——eligibility 13 处拒因收口 + `reject(名): 拒因` 日志（TPHP_JIT_DEBUG）；分布 = CONCAT×5 / DIV-MOD×3 / FE_RESET×1 / INIT_NEW×1。②**DIV/MOD 放行 ✓**（trace 实证编译器物化字面量 → t2 恒槽，旧 const 检查过严；emit 本就支持槽、零除走 slow+deopt）。③**CONCAT backend 排查完成、永久放行 ✓（10-29 第二批）**——根因非 RA 分层（O1 二分：coalesce/live_ranges 排除、O0 无罪、肇事=O1 reg_alloc 链下游）亦非上游已修（ir_ra.c 末次提交 10-19、dasc 快照后无相关修复）：**`ir_emit_arguments` 三 pass 乱序**——pass1 把 MEM/CONST→REG 装载推迟到 pass3，pass2 REG→REG 洗牌先行；当同一值作两个实参（`$o.=tmp` dst==src 同 ref，dst 的 ABI 目标 RDX 恰为另一实参 a 的源寄存器）时 `mov r8,rdx` 先于 `mov rdx,[spill]` → 读到上次 call 残留（gdb 实锤 r8=0x3f；LSRA 终表 ref8 段 [%rdx]+SPILL 并存；emit 端与 RA 端各自自洽、只差发射顺序）。修复 = **装载先行、洗牌后置**（x86+aarch64 × .dasc+预生成头四文件同调；安全性由 linear scan 为每个 arg reg 在 call load pos 建固定窗保证）。复现 tmp-s2 转正 `tests/lang/test-concat-loop-001.php`；**225/225 绿、每轮 ir_check=1、harness array-read 9.9ms 与记录持平**。附带同步上游 ir_emit.c 两修复（DESSA spill 双用 + parallel_copy 循环头类型；与本 bug 无关）。④**批2 INIT_NEW 对象链落地 ✓（10-29 第三批）**——架构=**回放真实调用序 opcode**（vm.c `tphp_jit_opseq`：bounded-pad 守卫 + jf->pc 驱动 + 帧切换深跑；`init_mcall_core` 从 op_init_mcall 抽函数零语义重复）：eligibility +4（INIT_NEW/NEW_USER/INIT_MCALL/UNSET_CV），emit 序折叠（嵌套/动态方法名 fail 回退、recv 快照、`lower_seq_replay` deopt 门无 dst store——真 handler 写回帧槽），raise 穿透链（未定义类/ctor throw）实测 catch✓。`tests/oop/test-new-jit-001.php` 6 例、**226/226 绿、ir_check=1、harness 无新增拒因**。**性能实话**：object 50.2-53.3 vs 52.5 持平——setjmp pad×2 + ctor/get 体仍解释；**兑现点=批2-2 prop ops 准入**（op_do_fcall vm.c:1668 回放内 callee jit_entry 自动直入，零改动吃 JIT→JIT）。⑤**批2-2 prop+方法准入 ✓（10-29 第四批）**——prop 四 op（FETCH/ASSIGN/ISSET/UNSET_PROP）走 `lower_replay(pc,pc+1)` 单 op pad 执行（真 handler 读自身操作数写 dst 槽，magic/raise 同门）；**解禁 is_method**（v1 布局债另一半）+ **修 op_do_fcall JIT 直入方法参数布局**（$this@slot0 参数1..，旧版直写 slots[0..n) 覆盖 $this——get/add 获 jit_entry 后实测暴露）+ **修 lower_incdec 忽略 dst**（只回写源槽：bump `return ++$this->n` 返回 UNDEF；补 dst 写时踩 store_imm 立即数坑=tag 存成 ref-id53，gdb 溯源 op_eq→to_float；suffix=标量直存+`tphp_jit_inc_olddst`（UNDEF→NULL+addref）/prefix=标量直存+tv_copy，**死 dst 保守跳写**归位 array-read 9.9→13.2→9.7ms）。`test-new-jit-001` 扩至8例 + trait 回归，**226/226 绿、三连跑 array-read 9.7-10.0/kv 51.5/object 同带回退清零**；object bench 仍 ~52 持平（setjmp pad×2+ctor 解释仍主耗）。⑥profile 观察计数机制本体（jsc-lessons §3）未动。⑦**批3 FE foreach 准入 ✓（10-29 第五批）**——FE 实名仅 `OP_FE_RESET`/`OP_FE_FETCH` 两 op（无 FE_NEXT/FE_FREE，游标/val/key 全帧槽）：eligibility +2、emit `lower_replay(pc,pc+1)` 单 op pad（FE_FETCH 即回跳目标=循环头，其后 JMPZ 由既有 loopcond 分支消费——**foreach 形态零新增 CFG 机械**）、**vm.c opseq 派发表 else-if 链 +2（关键易漏项：漏改则落"表达式跳过"分支静默丢语义）**。跨迭代状态（itmp/ctmp 游标/val/key/htmp）全在帧槽，逐 op 回放天然保持；ctmp tag 由 JIT CONST 存 T_INT、handler 只改 u.i 恒 INT。`tests/lang/test-fe-jit-001.php` 6 例（键值和/串键/break+continue/空数组/外 catch TypeError/循环内 raise），**227/227 绿、每轮 ir_check=1、三连跑 array-read 9.9/10.5/9.9 kv 52.5/52.1/52.2 object 持平噪声内**。**拒因归零核销：logstat 转拒 `ISSET_DIM (n_ops=106)`（FE_RESET 零出现）——ISSET_DIM 是拒因审计新一户，另行立项**。⑧**批4 ISSET_DIM 准入 ✓（10-29 第六批）**——`isset($a[$k])` 单 op pad（真 handler `rd` raw read = UNDEF 安全**零 raise**，无 deopt 门需求）三处同形态（eligibility+emit+opseq 派发表链+1）；`test-isset-dim-jit-001` 六例（命中/miss/UNDEF base/字符串偏移/循环内/动态 int 键）**228/228 绿、六函数全 `eligible=1`+`ir_check=1` 真编译**；perf stash A/B 三连对照 object 52.9-54.4 vs 基线 52.2-54.6 同带（中途 61.7 = 重建热态噪声）。**logstat 白名单拒因全清**（eligible=1），**新卡点 = emit `find_join` 拒 if 内回跳 continue（既有 CFG 限制，非白名单）——下一户立项**。⑨**批5 if 内 continue 的 emit CFG 支持 ✓（10-29 第七批）**——四堵墙连破：**(a) continue 回边**：find_join depth0 遇回跳 JMP 跳过扫 jends 定 join + 分支 TR_LOOPBACK→`ir_END()` 存 per-loop pending（lb_pend）按 trap_ret 走既有 RET 合流 + 体尾 trap_lb 处 `MERGE_N(pends+体尾)` 再 LOOP_END（**LOOP_BEGIN 保持单回边 pos2，零 deps/ir 改动**）；**(b) JMPNZ(||) 分支极性修正**：原 `ifx=NOT_B(truthy)` 极性反置 + tA=END@T 漏 emit 跳转目标体（该路径此前 100% fail 回退从未出错码），改 ifx 恒 truthy + 真侧 emit(T..join)/假侧 emit(pc+1..[T,join])；**(c) call_slots 参数 INT 契约守卫**：v1.2 "caller guarantees INT" 在非 INT 实参静态调用上首次被踩（logstat string 参数经 JIT 闭包静态调用→槽坏标签 `explode(): unknown given`）→ 入口 guard 返 0 走既有 deopt 链；**(d) jit_compile_callees 尊重急切/热阈值**：绕过阈值的连带编译把大 callee 编译成本塞进 caller 首调计时（logstat 106ops 连带编译 +85ms 实测 170→119ms）→ 阈值不满足不编、`!callee->jit_entry→fail` 既有兜底让 caller 回退。**三个安全挡板（fail 回退解释器，根修后放开）**：①嵌套 loop×FE（FE_RESET 被外层 loop 包裹→fail；O1 下 tv_copy 实参 src=0 崩，O0 分辨因 FOLDING 不兼容不可用——**独立 backend 户**）②循环内 || 结果喂 continue（lb_jmpnz 标记×lb_push→fail；ms1/ms2/ms5 最小复现，join/dump 全对仍值错=O1 codegen 域）③同层 pending ≥3（np≥2 挡；cont_multisite vs ms6 对照 key 布局改变结局）。**`tests/lang/test-loopcfg-jit-001.php` 8 例（continue 单/多/else/while/嵌套 + || 短路 + &&\|\|混合）**，**229/229 绿、每轮 ir_check=1、logstat 真编译（size=17120）且直跑输出正确**、主项同带（fib14/array9.9/kv52/object53）。**遗留账**：real-logstat bench 117 vs 基线101~~（+15，编译成本已移出计时、无新闭包编译，残差待查）~~ → **10-29 晚结案：残差不存在**——bench/CI 口径 logstat 恒解释器（单次调用<热阈值 127 永不编译）双态同速 ~52.5ms；"基线 101"=批3 FAILED emit 每调重编 ~49ms 摊进计时污染；117=单次异常读数未复现；300 次循环探针实测 **JIT 版 logstat 单次执行与解释器打平=白名单开路在 logstat 本体收益≈0**（诚实记录，不为 logstat 单点优化）；bench 项 make_log 实际计入计时（注释不实）=口径瑕疵待议。**新铁律：跨会话引用"基线"数字前必须验证产生口径** | 余量：①**if 内 break（前跳 Lend，emit 保守 fail 中）**②**O1 codegen 三新户**（嵌套 FE tv_copy、||×continue 值错、key×三 pending——均 ms/c7 最小复现）③|| 短路整体进 JIT（find_or_join 直取 jend 方案已试=compile 过值错已撤，需真值区/jend/合流整体重设计）④~~real-logstat +15 残差~~（已结案）⑤key+MOD/GT 槽 load 既有 bug（c7 无 continue 复现，批3 FE 准入暴露）⑥profile 机制⑦object 深化 | PLAN/jsc-lessons.md §3；src/jit.c、src/vm.c、deps/ir/ir_{x86,aarch64}.dasc+ir_emit_*.h；memory/2026-10-29.md |
| P0-15 | **call 特化 v1（10-29 晚，A/C/B 三切片落地）**：**A 静态直调**（jit_entry 运行期 load + 间接 ir_CALL 消 call_slots C 帧；INT 契约 IR 逐参 IF；**is_method→fail**=bug3 家族）；**C 帧合并**（call_dyn→tphp_jit_call 两层 C 帧合一 + n==nparams 防越界）；**B 闭包直调 FAST**（7 层检查链：T_CLOSURE/bfn/fn/n_cap/entry/nslots≤64/is_method → MERGE_N 慢块 call_dyn 兜底 → MERGE_2 合流读 ret_sc 免 phi；entry 块定额 64-slot scratch；callee prologue 自填 locals+tmps UNDEF）。**bug 三连全闭环**：①双重解引用（gdb 实锤 `LOAD_A(load_pay)`→`LOAD_A(clo_sc)`）②is_method 双侧（dyn if7 + static fail——批2-2 解禁后静态方法带 jit_entry，poff=1 布局）③**自填起点=fn->n_locals（n_locals 含参数！）**，原 n_params 覆盖 CV 区。**实测**：call 41.6→36.8-42.1（-9%）、kv-read 52→**39.3（-25%）**、array-read 9.9→**7.1（-28%）**、object 52.7→**41（-22%）**、fib 同带；**227/229**。**遗留 2 失败已修复 ✓（10-30 CI 定位）**：根因 = callee prologue 自填 tmps 的 entry store 被 O1 跨 FE replay call 折叠（gdb 实锤 $v=NULL，删即绿；哑参锚定无效）——**S1 终态 = 撤自填**，B 未初始化 CV 残留记账（正解 IR 动态循环填，慎入）；另两案（CV 区自填=方法 poff 起点错 18 失败、B 填满 64=126 store 吃光收益）实测撤回。同机 A/B 零性能回退，229/229。**批2 per-site fn 快照（10-30 落地 ✓）**：B 链前加门2——`op->ic` 直存闭包 `TFunc*` 单字（INIT_DYN 的 ic 勘察实证全库无读者，emit 哨兵 1），`clo->fn==ic` 命中即免 if2..if7 全链（fn 身份蕴含 bfn/fn/n_cap/entry/nslots/is_method 六项，TFunc 不可变 + jit_entry 写一次不清），链尾 1 store 填快照，热路径 2 分支+1 entry load。**229/229、ir_check 全 1、探针（换代/非闭包/捕获/MFCC）JIT vs 解释器逐行一致**。**性能如实记录：标准 bench call 打平（噪声带内）**——**发现先于本批的结构性缺口**：JIT'd caller 动态调用失败 if5（callee entry==0）→ `call_dyn→tphp_vm_call_callable` 直接建帧**绕过 op_do_fcall 的 hot++/eager 编译门**（vm.c:1662）→ callee 永不编译 → 链恒败于 if5 → 快照永无填充（插桩实证 50M 调用 0 命中 CHAINPASS）；基线 A 同样全程慢路径（P0-15 报告 36.8 本地不可复现的结构性解释）。**探针（callee main 层预编译使门2 可达）正反序双验 B 稳定快 -1.6%~-3.5%——机制有效，收益兑现依赖下述缺口修复**。**后续**：①**修 callee 编译缺口 ✓（10-30 续）**——`tphp_vm_call_callable`（call_dyn 慢路 / call_user_func / usort 族 / FFI 回调的统一 C 汇聚点）补 hot++/eager 门（与 op_do_fcall 1662 同式； ineligible/编译失败置 hot=UINT16_MAX 自动停表）。机制探针实证 callee 获 `compile() entry`（修复前该行永不出现）；**同轮 stash A/B：call 1e6 探针 58.0→7.9ms（-86%，7.3×）；全 13 项 bench 零回退，call 10.6 vs PHP 49.4 = 首次反超（0.21×）**。解释器侧动态调用（INIT_DYN→DO_FCALL）本就过 1662 门，无缺口 → **批3-3 静态方法直调 ✓（10-30 续）**：INIT_SMCALL 进白名单——`jit_smcallee` 编译期解析 op_init_smcall 的静态可判子集（self::=decl_cls/parent::/字面量 Cls::m；**static::/动态名/enum builtin/native/__callStatic/__construct/实例转发全拒=解释器完整语义**；可见性用同一 tphp_vis_ok、from=decl_cls 恒不松于运行期 scope），结果缓存 op->ic（op_init_smcall 零读者直用，批2 同款）+ jit_compile_callees 同扫；emit 复用 A 直调机械（poff=1：slot0 UNDEF + 参数 1.. + locals UNDEF 填充）；**vm.c opseq 派发表补 INIT_SMCALL**（防回放序列内静默跳过）。对拍收获：**PHP `__callStatic` 对"不可访问"（非仅未定义）静态方法同样生效**——test-smcall-jit-001（13 例双端逐字节一致）固化。230/230、ir_check=1、bench 零回退 | call 已进反超带；下一户=serialize/json/throw/logstat（2.4-5.3×，**serialize 实测=双侧均匀 ~3.5×=分配/GC 结构差（tstr_new+gc_register vs emalloc），非单点算法**）与 object/push/rmw（2.3-2.7×） | src/jit.c、src/vm.c；memory/2026-10-29.md、2026-10-30.md |

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
