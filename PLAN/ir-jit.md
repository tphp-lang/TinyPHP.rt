# ir-jit · IR JIT v1 规格（baseline，函数级）

> 状态：**v1.2 已实现（2026-10-27）**——direct-slot ABI（`JitEntry(vm,slots,ret)`，JIT→JIT 直调免 TLS trampoline + 去 prologue guard/copy）；fib(28) **14.0ms vs PHP tracing JIT 10.9ms ≈ 1.27×**；链式 TMP-ADD 崩溃已修（O1 跳 SCCP，deps/ir 多槽位 CALL bug）；急切编译 `n_ops<=96` 首调即编；全量 224/224。上级索引：[../PLAN.md](../PLAN.md)；战略背景见 [perf-roadmap](perf-roadmap.md) §1.1/§5。spike 先行验证（tools/ir_spike_fib.c）；SCCP 诊断工具 tools/ir_bisect.c。

## 1. 目标与 v1 范围

**目标**：热用户函数的函数级 baseline JIT——opcode→IR lowering + 类型 guard，优化/寄存器分配/机器码由 `deps/ir`（dstogov/ir，与 PHP 8.4+ opcache JIT 同后端）全权负责。

**v1 范围**（M1）：
- 函数级编译，无 OSR（只 JIT 从入口进入的调用；循环型函数下次整函数调用即走 native，够用）
- **封闭类型世界**：函数体只允许 int/float/bool/null 域值（见 §5 资格判定）——无 string/array/object/闭包/异常/生成器/FFI
- 调用：仅静态解析的、同样通过资格判定的用户函数（fib 式递归/互递归）
- **首发平台**：Windows/Linux/macOS **x86_64 + aarch64**（IR 双后端，emit 头已预生成入库：`ir_emit_x86.h` / `ir_emit_aarch64.h`）

**非目标（v1 明确不做）**：OSR、trace/侧路追踪、inline cache、对象/数组/字符串运算、异常路径 JIT（体内 raise 一律排除，见 §7）、闭包/方法调用、类型推断式的全 SSA 传播（交给 IR 的 SCCP）。

## 2. 总体结构

```
DO_FCALL 用户函数路径（帧已建、RECV 已跑、slots 就绪）
  └─ fn->jit_entry != NULL？
       ├─ 是 → jit_entry(vm, slots, &ret) == 1 → ret 写目标槽（跳过解释器）
       │        └─ == 0（prologue guard 失败）→ 回落解释器 run 循环
       └─ 否 → fn->hot++；达阈值 → tphp_jit_compile(fn)（同线程同步编译，v1）
```

- **编译时机**：同步、达阈值一次（失败熔断 `hot = UINT16_MAX` 不再重试）
- **阈值**：对齐 Zend 默认 `jit_hot_func = 127`（函数入口计数；fib 递归 2^29 次入口，首帧内即触发）
- **计数点**：`op_do_fcall` 用户函数路径 + `tphp_vm_call_callable` 用户函数分支（覆盖 `$f()`/call_user_func 入口）

## 3. ABI 与数据交换

```c
/* TFunc 增加字段 */
uint16_t hot;          /* 入口计数；UINT16_MAX = 编译已尝试且失败（熔断） */
void    *jit_entry;    /* JitEntry 或 NULL；写一次全局只读（线程安全见 §10） */

typedef int (*JitEntry)(VM *vm, TVal *slots, TVal *ret);
/* v1.2 direct-slot ABI：
 *   slots[0..n_params-1] = 参数（JIT→JIT 调用方保证 INT；解释器路径在 C 侧 guard）
 *   slots[n_params..]    = locals/tmps（调用方初始化为 UNDEF）
 *   返回 1 = 已计算 *ret（owned by caller）；0 = deopt（解释器重跑） */
```

- `slots` 布局与 VMFrame 完全一致：`[captures..][this?][params][locals][tmps]`（TFunc 槽位约定不变）
- JIT 代码直接读写 TVal 内存槽（16B：`{union u; TType t}`）；块内寄存器中用拆开的 tag/payload 表示，进出槽位时装箱
- **v1.2 直调路径**：JIT→JIT 调用在 IR 内 ALLOCA callee slots、参数直写、经 `tphp_jit_call_slots` 运行时查 `jit_entry` 后直调——无 TLS trampoline、无 prologue guard/copy（fib 20.8ms→14.0ms）
- 解释器→JIT 路径在 C 侧 guard 参数 INT + 拷贝 args→slots 后调 entry；guard 失败回落解释器

## 4. 体内的值表示

封闭世界内每个活跃值 = `{ tag: i8, payload: i64 }`，tag ∈ {NULL, FALSE, TRUE, INT, FLOAT}：

- INT payload = i64；FLOAT payload = double 的位模式（运算前 bitcast）
- 槽位装箱：tag→TType 直映（i8 常量表），payload 写 `u.i`/`u.d`
- 算术/比较的 int 快路 + float/混合慢路（§6）；**体内不做逐 use tag guard**——资格判定（§5）保证类型不出域，tag 只在装箱与分支快慢路选择时被读

## 5. 资格判定（tphp_jiteligible）

编译前静态扫描 `TFunc`，全部满足才编译：

1. **opcode 白名单**（§6 列表外任一 op → 不合格）
2. **lit 限定**：被引用的 `lits[]` 条目类型 ∈ {int,float,bool,null}（CONST 进入封闭世界的唯一外部源）
3. **无异常表**（`n_ex == 0`）且非 generator/closure/method/main/variadic/is_global 槽
4. **DIV/MOD 限制**：出现即要求除数为编译期常量且非零（否则可能 raise，JIT 帧上 longjmp 与 FFI 回调同类问题 → 不合格）
5. `n_params ≤ 8`（prologue guard 展开上限；后续可放宽）
6. 非递归限定放开——允许任意调用白名单内函数（含自身/互递归）；调用目标在 INIT_FCALL 的 ic 已解析为用户 TFunc 且该 TFunc 也合格（惰性：被调方未合格则本函数也不编译）

## 6. opcode → IR lowering（v1 白名单）

| opcode | lowering |
|---|---|
| OP_CONST | 从 `lits[a1]` 装箱 tag+payload（§5-2 保证已数值域） |
| OP_COPY | tag+payload 槽间搬运 |
| OP_ASSIGN | 源→目标槽装箱写入（先 release 旧值：见 §7 所有权） |
| OP_ADD/SUB/MUL | 全 INT → 有符号溢出检查（IR carry/overflow flag）；溢出分支 → 转 FLOAT 继续（PHP 语义 int→float 提升）；任一非 INT → slow helper（§8） |
| OP_NEG/POS/INC/DEC | 同上（INC/DEC 用 checked add ±1） |
| OP_DIV/MOD | 仅常量非零除数（§5-4）：int 除法按 PHP（整除 trunc、mod 符号对齐 C99 与 php-src 一致——负数用例进对拍测试） |
| OP_POW | 不进 v1 白名单（int 次幂结果可能 float，语义面大）——v1.1 |
| OP_BW_AND/OR/XOR/OP_BIT_NOT | payload 直接位运算（tag 必为 INT，资格保证） |
| OP_SHL/SHR | 仅移位量为**编译期常量**：编译期按 PHP 语义归一（负数反向、≥64→0）；变量移位量 → 不合格（v1.1 slow helper） |
| OP_BOOL_NOT | NULL/FALSE→TRUE；INT 0/FLOAT 0.0→TRUE（tag+payload 判零）；else FALSE |
| OP_BOOL_VAL | 强制转 bool 同上判零规则 |
| OP_CAST | 仅 (int)/(float)/(bool) 且源 tag ∈ 域内：int←float trunc、float←int、bool 同上；(string) 等不合格 |
| OP_EQ/NE/SEQ/SNE/LT/LE/CMP | INT×INT 直接比；混 FLOAT→slow helper（域内 NULL/FALSE/TRUE 数值化规则由 helper 按 php-src 执行——对拍钉死）；`===` 全域严格（tag 先比） |
| OP_JMP/JMPZ/JMPNZ | IR 分支/跳转；JMPZ/NZ 用 PHP 真值规则（§ BOOL_NOT 同款判零） |
| OP_RECV / OP_SEND_VAL / OP_SEND_VAR | DO_FCALL 帧协议的槽搬运——**v1 调用约定**：RECV/SEND 仍由解释器在进 JIT 前跑完（§2 时序）；JIT 函数体内不出现这三者（参数已就位）。跨 JIT→JIT 调用走 §8 helper 直调 |
| OP_INIT_FCALL | 不进函数体白名单（调用展开见 §8——v1 函数体内的调用形态由资格判定限定为"ic 已解析且 callee 合格"，lowering 时 INIT+SEND+DO_FCALL 三元组折叠成一条 IR call） |
| OP_DO_FCALL | 折叠形态：IR 调 callee 的 jit_entry（经 §8 trampoline）；返回值装箱进 tr 的槽 |
| OP_RETURN | 装箱返回值 → 写 *ret；帧槽释放策略见 §7；返回 1 |
| OP_ISSET_CV / OP_UNSET_CV | ISSET：槽 tag ≠ T_UNDEF/T_NULL…按 php-src isset 语义（域内 = 非 null 且非 undef）；UNSET：置 T_NULL（域内封闭） |

**折叠规则**：`INIT_FCALL + SEND_VAL/VAR×k + DO_FCALL` 在 lowering 期识别为一个调用点（编译期线性扫描），展开为对 callee `jit_entry` 的直接 IR call（无 VMCallRec）。非此形态（动态名/混 builtin/闭包）→ 不合格。

## 7. 语义与所有权（对拍钉死）

- **int 溢出 → float**：checked add/sub/mul，溢出分支 IR 内转 double 运算（对拍 `PHP_INT_MAX + 1`）
- **除法/mod 符号语义** = php-src `div_function/mod_function`（负数用例进 tests/jit 对拍）
- **槽位所有权**：JIT 写槽前对旧值 `tval_release`（经 C helper 调用，域内旧值只有 INT/FLOAT/BOOL/NULL = no-op，但保持协议统一防将来扩域踩坑）；返回值 TVal 的所有权 = +1 移交 DO_FCALL（与解释器 RETURN 一致）
- **GC**：JIT 代码不分配堆对象（封闭世界），不触 GC；slots 中的值全为标量，gc_arr_children 等无需感知
- **异常**：体内不可能 raise（§5-4 + 白名单）；`tphp_vm_raise` 不出现在 JIT 代码路径

## 8. 调用约定（JIT→JIT）

```c
/* 每个被编译函数的真实入口；DO_FCALL 与 JIT 调用点共用 */
int vm_jit_entry_thunk(VM *vm, TVal *slots, TVal *ret);
```

- JIT 内 call：直接 `ir_CALL` callee 的 `jit_entry`（编译期已知函数指针，经 IR const addr）；callee prologue guard 失败 → 返回 0 → **调用方 JIT 代码将本函数也整体 deopt**：v1 规则简化为"合格链上任一 callee 也是静态合格的，其 prologue guard 只检查参数 tag——调用方传入值域封闭 ⇒ guard 必过"，故 v1 把 callee guard 失败视为不可达；若未来破域，v1.1 再引入逐调用点 deopt
- 默认参数/variadic：RECV 已由解释器完成（§6），JIT 入参即最终槽位值
- `tphp_vm_call_callable` 等间接入口不进 JIT（v1 调用只有 DO_FCALL 折叠形态）

## 9. slow helper（C 侧，`src/jit.c`）

域内混合类型运算与非平凡语义统一走 helper（不 raise，纯计算）：

```c
void tphp_jit_binop_slow(VM *vm, int op, TVal a, TVal b, TVal *out);
void tphp_jit_cmp_slow(VM *vm, int op, TVal a, TVal b, TVal *out);
```

实现直接复用解释器现有 `tval_add/…/tval_compare` 家族（语义单源，杜绝第二套规则）。

## 10. 构建、平台与线程

- **构建**：IR 源 16 个白名单（spike 同款）编入 `build/obj_ir/`（**绝不进 build/obj/**——通配链接会与 src_main 冲突）；`src/jit.c` 进常规 obj；链接行显式追加 `build/obj_ir/*.o` + `tools/chkstk_stub.S`；无 libgcc 机器上 chkstk 桩必需（spike 教训）
- **运行时禁用**：`TPHP_JIT=0` 环境变量关闭；x86_64 与 aarch64 均支持（IR 双后端）
- **线程**：prog freeze 后跨 Worker 只读共享 ⇒ `TFunc.jit_entry` 全局一次写。v1 编译加**进程级 mutex** 串行化（防同 fn 双编译泄漏 code page）；生成码只读执行
- **code 内存**：`ir_jit_compile` 返回的页由 IR 管理；进程退出随 OS 回收（CLI 场景足够；常驻 server 再议）

## 11. 测试与验收

- `tests/jit/`：①正确性——fib/ackermann 递归、int 溢出提升、负数 div/mod/shl/shr、比较混合 tag、isset/unset、与解释器同脚本双路径对拍（`TPHP_JIT=0/1` 各跑一遍 EXPECT 相同）②资格否定——含 string/数组/DIV 变量除数的函数必须仍走解释器且结果正确
- 对拍基准 `tools/bench-vs-php.php`：**M1 达标线 = fib vs PHP tracing JIT ≤2.5×**（当前解释器 10.5×；spike 上界 0.11×，TVal 装箱+guard+call 开销后预期 3–8ms）。call（闭包）不在 v1 白名单，M1 不考核；object/数组类等 v1.1
- CI：5 平台照旧（JIT 默认开；aarch64 job 自动禁用不红）

## 12. 里程碑

1. **v1.0**：§1–§10 全量——fib 达标（M1）
2. **v1.1 候选**：POW/变量移位 slow helper、float 参数 prologue、闭包调用（SEND_CAP/CLOSURE_INIT 域化）、FETCH_DIM_R packed 快路进白名单（array-read 敏感簇）
3. **v1.3 候选**：对象属性读（单态类 guard）、编译异步化

## 13. 风险与不做清单（防翻案）

| 项 | 原因 |
|---|---|
| 体内异常路径 JIT | JIT 帧上 longjmp 与 FFI 回调同类问题（C 栈穿越）；资格判定整体排除 |
| v1 即做数组/字符串 | 语义面（COW/引用计数/in-place）×guard 组合爆炸；先用封闭世界把 lowering 框架与 ABI 打磨硬 |
| 自研 regalloc/codegen | ir 的 Sea-of-Nodes + linear scan 就是买来的部分；胜负在 lowering 质量 |
| 与解释器分两套语义实现 | slow helper 一律复用 tval_* 家族（§9） |
