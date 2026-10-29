# spec · P0-14 批3：FE_RESET / FE_FETCH foreach 准入

> 2026-10-29 拒因审计最后一户：`TPHP_JIT_DEBUG` 下 `reject(logstat): FE_RESET`。
> 本文为决策完整规格；执行账见 [tasks.md](tasks.md)。批准后按账执行。

## 1. 目标与验收

让含 foreach 的函数通过 JIT 准入并正确执行（FE 两 op 进 JIT 编译），

**验收（全部满足才算完）：**
1. `powershell -ExecutionPolicy Bypass -File tests/run.ps1` 全绿（当前 226，本批新增测试 → **227**）。
2. 每轮编译盯输出 **ir_check=1**（铁律，>1 即编译期结构错误）。
3. `TPHP_JIT_DEBUG=1` 下对含 foreach 的 JIT 准入函数**不再出现 `reject(...): FE_RESET`**。
4. perf sanity：`build\tphp.exe run tools\bench-vs-php.php` 三连跑，array-read ~9.9、kv ~51.5 无回归（波动>5% 三连复测）。
5. 无 TEMP/JIT debug 打印残留（定位用打印用后删）。

**非目标（本批不做）：**
- `OP_ISSET_DIM` 等 logstat 其余拒因——logstat 可能在 FE 通过后**转拒于 ISSET_DIM**，那是独立一户，另行立项，本批不顺手扩。
- Generator 对象 foreach 进 JIT（Generator 无法流入 JIT 帧：`jit_eligible_inner` 拒 `is_generator` callee，参数守卫 INT——分支存在但不可达，不专门测试）。
- FE 序列为编译期展开（保持逐 op 回放形态）。

## 2. 现状勘察（已核实，勿重查）

| 事实 | 位置 |
|---|---|
| FE 实名仅 `OP_FE_RESET`/`OP_FE_FETCH` 两 op（无 FE_NEXT/FE_FREE） | src/opcode.h:35 |
| 字节码形态：`FE_RESET arr→itmp`；`CONST 0→ctmp`（预留 +1 val/+2 key）；`Lstart: FE_FETCH itmp,ctmp→htmp`；`JMPZ htmp→Lend`；`COPY ctmp+1→valCV`；`COPY ctmp+2→keyCV`(可选)；体；`JMP Lstart` | src/compile.c:2341-2366 |
| `op_fe_reset`：rdv 读操作数 → 非数组非 Generator 抛 TypeError → addref+wtake 写 itmp | src/vm.c:865-883 |
| `op_fe_fetch`：读 itmp（rd）、游标 `opd(op,2)->u.i`（只改 payload 不改 tag）、写 `slots[a2+1]/slots[a2+2]`、wtake 写 htmp；耗尽写 FALSE | src/vm.c:885-927 |
| raise 落 `tphp_jit_opseq` 有界 pad（setjmp 登记/escaped→返 0/重注册继续）——与 prop 同款 deopt 门 | src/vm.c:2332-2388 |
| opseq 帧切换深跑循环已存在（承接 handler 内帧切换） | src/vm.c:2352-2363 |
| **opseq 派发 else-if：不在真 handler 链表的 op 落 `d=1` 表达式跳过分支**——FE 必须加进链表，否则回放静默丢语义 | src/vm.c:2370-2378 |
| eligibility 白名单 switch，`default:` 拒因输出（主审计目标） | src/jit.c:567-680 |
| emit_seq：回跳目标自动建 LOOP_BEGIN + `loop_cond=1`；**首个 JMPZ 被当循环条件**；体尾回跳 → trap_lb → LOOP_END——foreach 形态零适配 | src/jit.c:1538-1546、1867-1898 |
| emit 缺 case → default → trap_fail → 不编译回退解释器（安全） | src/jit.c:1978 |
| `lower_replay(j, pc_start, pc_end)`：发 `tphp_jit_opseq` 调用 + IF ok==0 → RETURN 0（deopt 门），无 dst store、无 MERGE | src/jit.c:1313-1324 |
| prop 四 op 同形态范例（eligibility src/jit.c:673-676、emit src/jit.c:1766-1775） | 批2 已落地 47e0fa2 |
| ctmp 游标 tag：JIT OP_CONST 存 T_INT，handler 只改 u.i → 恒 INT 读取成立 | src/jit.c:1550-1561 |
| logstat 拒因 = FE_RESET（本批）；其 `isset($counts[$ip])` 用 OP_ISSET_DIM 不在白名单（转拒后续） | tools/real/logstat.php:36 |

## 3. 改动（恰三处 + 测试 + 文档）

### 3.1 src/jit.c — eligibility 白名单加 case
在 prop 四 op case（~line 673）旁加：
```c
case OP_FE_RESET: case OP_FE_FETCH:
    /* P0-14 批3：foreach 单 op pad 执行（emit 见 lower_replay）——
     * 真 handler 读自身操作数写帧槽（itmp/ctmp 游标/val/key/htmp
     * 全在帧槽，跨迭代回放保持），raise 走 opseq deopt 门 */
    break;
```
不加操作数形态限制：FE_FETCH 三操作数恒 TMP（compile.c 固定），FE_RESET 的 arr 操作数由真 handler 自读（CONST/CV/TMP 皆可，`rdv` 处理）。

### 3.2 src/jit.c — emit case
在 prop emit case（~line 1766）旁加：
```c
case OP_FE_RESET: case OP_FE_FETCH:
    /* P0-14 批3：单 op pad 执行；FE_FETCH 即回跳目标=循环头，
     * 其后 JMPZ 由既有 loopcond 分支消费（形态零适配） */
    lower_replay(j, pc, pc + 1);
    pc++;
    break;
```

### 3.3 src/vm.c — opseq 派发表加两 op（**关键且最易漏**）
`else if` 真 handler 链（line 2370-2374）追加 `o->op == OP_FE_RESET || o->op == OP_FE_FETCH`。
否则两 op 落 `d = 1`（表达式跳过）→ 回放静默丢 foreach 语义（不报错、结果错）。

### 3.4 新增测试 tests/lang/test-fe-jit-001.php（+1 → 227）
覆盖：①键值求和 foreach（正路，走 `COPY ctmp+1/+2`）②`break`/`continue` 体内跳转 ③空数组遍历（FE_FETCH 首次即 FALSE）④非数组 foreach 的 TypeError 在 JIT **外** catch（验 deopt 传播链，仿 test-new-jit-001 的 new_undefined 姿样）⑤foreach 前值非 UNDEF 的计数结果与解释器对拍（EXPECT 注释定死）。
（文件是否已存在 foreach 测试可先 glob；有则扩既有文件而非新建。）

### 3.5 文档同步
- PLAN/backlog.md P0-14 行：批3 结果追加（含 logstat 转拒 ISSET_DIM 的后续项记录）。
- memory/2026-10-29.md：批3 追加（改动点、验收数字、教训）。

## 4. 验证步骤（按序）

1. `cmd /c build.cmd` → 盯 ir_check=1。
2. `powershell -ExecutionPolicy Bypass -File tests/run.ps1` → 227/227。
3. `TPHP_JIT_DEBUG=1 build\tphp.exe run tools\real\logstat.php` → 观察 reject 行：FE_RESET 消失（若转拒 ISSET_DIM = 符合预期，记录之；若 logstat 干脆全过 = 更好）。
4. 编写期可用最小 foreach 脚本 + `TPHP_JIT_DEBUG=2` 确认编译成功路径（用后删打印）。
5. `build\tphp.exe run tools\bench-vs-php.php` 三连跑 sanity（array-read ~9.9 / kv ~51.5）。
6. 文档两处同步 → 停，**等用户明说"提交"才 commit**（英文单域前缀 `jit:`）。

## 5. 风险与回退

| 风险 | 处置 |
|---|---|
| 漏改 3.3 → 结果静默错误 | 3.4 测试①②⑤覆盖结果比对，必现 |
| FE_RESET raise（TypeError）deopt 链 | 3.4 测试④ catch 验证 |
| 循环内 deopt（体中 raise）pc 归因 | opseq `jf->pc = vpc` 已归因；测试④在循环内 raise 变体覆盖（foreach 体中调用抛错） |
| 性能回归 | 步骤 5 三连跑 |
| logstat 仍不进 JIT（转拒 ISSET_DIM） | 非本批目标，backlog 记录后续 |
