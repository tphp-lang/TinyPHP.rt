# tasks · P0-14 批3（FE foreach 准入）

规格：[spec.md](spec.md)。按序执行，每步完成后勾账。

- [x] T1 src/jit.c eligibility：prop case 旁加 `case OP_FE_RESET: case OP_FE_FETCH: break;`（spec §3.1）
- [x] T2 src/jit.c emit：prop case 旁加同名 case → `lower_replay(j, pc, pc + 1); pc++; break;`（spec §3.2）
- [x] T3 src/vm.c:2370-2374 opseq 派发 else-if 链追加 `|| o->op == OP_FE_RESET || o->op == OP_FE_FETCH`（spec §3.3，关键易漏项）
- [x] T4 build：`cmd /c build.cmd`，盯 ir_check=1（PS 直跑 `.\build.cmd` 等价，Build OK；警告均为既有 pcre/string 项）
- [x] T5 测试：先 glob 是否已有 foreach JIT 测试 → 有则扩、无则建 tests/lang/test-fe-jit-001.php，五场景（键值和/break+continue/空数组/外 catch TypeError/循环内 raise）（spec §3.4）
- [x] T6 全量 `powershell -ExecutionPolicy Bypass -File tests/run.ps1` → 227/227（禁改期望凑绿）
- [x] T7 拒因验证：`TPHP_JIT_DEBUG=1 build\tphp.exe run tools\real\logstat.php` → FE_RESET 拒因消失；转拒 ISSET_DIM 属预期，记录（实测 `reject(logstat): ISSET_DIM (n_ops=106)`，FE_RESET 零出现；触发需 bench 形态 200 次调用过热阈值）
- [x] T8 perf sanity：`build\tphp.exe run tools\bench-vs-php.php` 三连跑，array-read ~9.9 / kv ~51.5 无回归（实测 array-read 9.9/10.5/9.9、kv 52.5/52.1/52.2、object 51.5/52.1/52.3，噪声内 all-ok）
- [x] T9 清理：JIT debug 定位打印零残留（TEMP 标记惯例）（本批零新增打印；探针落系统 TEMP 不入仓）
- [x] T10 文档：PLAN/backlog.md P0-14 行 + memory/2026-10-29.md 批3 追加
- [x] T11 停：等用户明说提交（英文单域前缀 jit:）

## 批4：ISSET_DIM 准入（同日续，用户"下一步吧"）

- [x] B4-1 三处改动：jit.c eligibility `case OP_ISSET_DIM`（rd raw read 零 raise）+ emit `lower_replay(pc,pc+1)` + vm.c opseq 派发表链 +1
- [x] B4-2 构建 Build OK
- [x] B4-3 测试 tests/lang/test-isset-dim-jit-001.php 六例（命中/miss/UNDEF base/字符串偏移/循环内/动态 int 键）→ **228/228 绿**
- [x] B4-4 JIT 真编译验证：六函数 `eligible=1` + `ir_check=1` + compile entry 全出（TPHP_JIT_DEBUG 探针，无残留）
- [x] B4-5 perf A/B：stash 对照重建 + 三连跑，object 52.9-54.4 vs 基线 52.2-54.6 同带（中途 61.7 为重建热态噪声，A/B 排除回归）；array-read ~10.0 / kv ~52 同带
- [x] B4-6 logstat 拒因推进：`eligible(logstat)=1`（白名单拒因清零）→ **新卡点 = emit `find_join` 拒 `if($ln==="") continue;` 回跳 JMP（pc=17 JMPZ）**——独立既有 CFG 限制（非白名单），单独立项
- [x] B4-7 文档：backlog P0-14 ⑧ + memory/2026-10-29 批4 追加
- [x] B4-8 停：等用户明说提交

## 批5：if 内 continue 的 emit CFG（同日续，用户"下一步吧"）

- [x] B5-1 find_join 回跳 JMP 跳过扫描 + if 分支 TR_LOOPBACK→lb_pend pending + trap_ret 合流复用
- [x] B5-2 体尾 trap_lb MERGE_N(pends+体尾)→LOOP_END（单回边 pos2 不变，零 deps/ir 改动）
- [x] B5-3 JMPNZ(||) 极性修正（原 NOT_B 反置+漏 emit 跳转目标体，此前 100% fail 回退）
- [x] B5-4 崩溃排查（嵌套 FE tv_copy src=0，gdb 实锤）→ **挡板①**：FE_RESET 要求 loop_sp==0 / FE_FETCH 要求 sp==1 且自身头
- [x] B5-5 O0 分辨证伪（ir_jit_compile(O0)×FOLDING 恒 NULL=全解释器假象，教训记档）
- [x] B5-6 ||×continue 值错（join dump 全对仍错=O1 codegen 域，ms1/ms2/ms5 复现）→ **挡板②** lb_jmpnz×lb_push；find_or_join 方案试撤（compile 过值错）记 backlog
- [x] B5-7 key×三 pending 值错 → **挡板③** lb_push 同层上限 2
- [x] B5-8 call_slots 参数 INT 契约守卫（logstat string 参数经 JIT 闭包静态调用槽坏标签→ deopt 链）
- [x] B5-9 jit_compile_callees 尊重 EAGER/HOT 阈值（连带编译 +85ms 进 bench 计时，170→119）
- [x] B5-10 tests/lang/test-loopcfg-jit-001.php 8 例 → **229/229 绿**、每轮 ir_check=1
- [x] B5-11 bench 三连：主项同带（fib/array/kv/object）、logstat 真编译（size=17120）直跑正确；real-logstat +15 残差记账
- [x] B5-12 清理：全部 TEMP 撤净（deps/ir/ir.h 零 diff、grep 0 残留）
- [x] B5-13 文档：backlog ⑨ + memory/2026-10-29 批5
- [x] B5-14 停：等用户明说提交（批3+4+5 一并，jit: 前缀）
