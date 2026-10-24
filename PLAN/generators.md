# generators · 生成器（yield）

> 状态：**G1+G2+G3 已完成**（2026-10-24，210/210；v2 后置待续）。
> 语义基准：php-src 8.5.7（zend_generators.c / zend_compile.c / zend_vm_def.h；分析记录见会话日志）。
> 性能路线关联：[perf-roadmap.md](perf-roadmap.md) §3 —— 生成器属"语法阈值"判据的一部分，先于解释器优化与 JIT。

## 1. 定位与核心决策

| 决策点 | 结论 |
|---|---|
| 实现模型 | **帧保存**（PHP 同构）：生成器 = 挂起的 VMFrame 存于 Generator 对象。tphp 的 VMFrame{fn,pc,slots,prev,file} 自包含，比 zend 的 execute_data+VM stack 更简单——**不**用协程实现（生成器是冷的、显式驱动、不进调度器；协程是热的） |
| 与协程关系 | 独立机制。禁止混用（await 不驱动 Generator；yield 不挂起调度器） |
| `&$ref` | 永不支持（全局决策）；by-ref yield / by-ref foreach 一并不做 |
| Generator 类 | 内部类、final（不可继承/clone/序列化——文档约定 + 无 new） |
| v1 范围 | 见 §5；`yield from`、`throw()`、dtor-跑-finally 明确后置 |

## 2. 编译期

- **词法**：`lex.c` 加 `KW_YIELD` / `KW_FROM`（`yield from` 连用识别在 parse）
- **标记**：parser 遇 `yield` 表达式 → 当前编译函数置 `is_generator`（tphp 对应 ZEND_ACC_GENERATOR）。加载期拒绝：函数体外的 yield（"yield expression can only be used inside a function"）；参数默认值 / 类常量 / 属性默认值表达式中的 yield（constexpr 上下文，PHP 同禁）
- **返回类型校验**（加载期）：生成器函数的返回类型必须为空或 `Generator`/`Traversable`/`Iterator`/`object` 之一（PHP 规则）
- **opcode**（3 个动作 + 1 个标记位）：
  - `OP_GENERATOR_CREATE`——编译器插在生成器函数体首条；帧已由 DO_FCALL 建立挂链，本 op 把帧摘下装入 Generator 对象并把对象写回调用方 dst，然后返回控制
  - `OP_YIELD`——产出：值 t1、键 t2（可空）、send 目标 dst=tr
  - RETURN 路径——`fn->is_generator` 时走"存 retval、关生成器、销毁帧"分支（不新增 opcode）
  - `yield from` → 后置（v2）
- **TFunc** 加 `bool is_generator`；**AST** 加 `A_YIELD`（值/键表达式）
- 生成器函数的调用点**无特殊处理**（普通 INIT_FCALL/DO_FCALL；首条 GENERATOR_CREATE 完成"建而不跑"）

## 3. 运行期

### 3.1 Generator 对象（内部类）

字段（存对象槽或注册表，同 FFI/Closure 模式）：

```
frame   VMFrame*        —— 挂起的帧（NULL=已关闭）
state   AT_FIRST | RUNNING | AT_YIELD | CLOSED
value   TVal            —— 最近产出值
key     TVal            —— 最近产出键
retval  TVal            —— 函数 return 值（未返回=T_UNDEF）
send_dst 载体           —— yield 的 dst（帧槽位下标；恢复时 send 值写入）
auto_key int64          —— 自动键计数（初值 -1；PHP：无键 yield 用 ++；显式整数键则 large=该键）
```

### 3.2 生命周期

- **创建**：DO_FCALL 建帧后执行 `OP_GENERATOR_CREATE` → 帧从主链摘下（prev=NULL）、args 区按现有规则回水位、Generator 持帧，state=AT_FIRST
- **首个驱动**（rewind/next/send/foreach 的第一次）：把帧挂回（prev=驱动方帧），从 pc=0 跑到第一个 `OP_YIELD` 或帧结束
- **OP_YIELD**：value/key 写入 Generator；`auto_key` 规则同 PHP；无更多执行（帧保留、prev=NULL、state=AT_YIELD）；控制交回驱动方
- **next()/send()**：state 校验（RUNNING 重入 → Error "Cannot resume an already running generator"；CLOSED → next/send 静默无操作、send 返回 null）；send(v) 先把 v 写入 yield 的 dst 槽；挂帧 resume（pc 已过 YIELD）跑到下一个 YIELD/RETURN
- **函数 RETURN**（is_generator）：retval 存 Generator、state=CLOSED、销毁帧（frame_destroy——槽位引用正常释放）
- **GC**：Generator 对象 GC 回收时若帧未关 → 直接 frame_destroy（**不跑 finally**——§6 已知差异）

### 3.3 foreach

`op_fe_reset` / `op_fe_fetch` 增加 **Generator 对象分支**（v1 不做一般 Iterator/Traversable）：

- FE_RESET：目标是 Generator → addref 存入迭代 tmp；驱动到首个 yield（rewind 语义）；已关闭 → Error "Cannot traverse an already closed generator"
- FE_FETCH：Generator 未耗尽 → current/key 写入 as 变量、next 驱动、true；耗尽 → false

### 3.4 Generator API（v1）

| 方法 | 行为（对齐 PHP，见会话日志 §3） |
|---|---|
| `rewind()` | 初始化跑到首个 yield；已运行过 → Error "Cannot rewind a generator that was already run" |
| `valid()` | frame != NULL |
| `current()` / `key()` | 未定义时 null |
| `next()` | 初始化（若需要）+ 恢复；CLOSED 静默 |
| `send($v)` | 写 send_dst + 恢复；返回新 current；CLOSED 静默返回 null |
| `getReturn()` | 未返回 → Error "Cannot get return value of a generator that hasn't returned" |

v2：`throw($e)`（暂停点注入异常）、一般 Traversable 的 foreach。

## 4. 允许 / 禁止（v1）

允许：实例/静态方法、闭包、箭头函数（`fn() => yield`，箭头体自动成生成器）、构造器内、try/catch/finally 内的 yield（帧内 try 状态随帧保存）、`yield $k => $v` 显式键。

禁止（加载期报错）：函数体外；参数默认值/属性默认值/类常量表达式内；（by-ref 全局永不）

## 5. 实施分批

| 批 | 内容 | 验收 |
|---|---|---|
| **G1 基础** | lex KW_YIELD + parse A_YIELD + is_generator 标记 + 加载期禁止面 + OP_GENERATOR_CREATE/OP_YIELD + Generator 类（rewind/valid/current/key/next）+ RETURN 关闭分支 + foreach Generator 分支 | foreach 值遍历、自动键、提前 return 后 valid=false；test-generators-basic.php |
| **G2 send/键** | send() + 显式键 + auto_key 更新规则（显式整数键后 auto 继续递增） | test-generators-send.php |
| **G3 getReturn/边界** | getReturn + 返回类型校验 + 已关闭生成器 foreach 错误 + rewind 重跑错误 | test-generators-edge.php |
| **v2 后置** | `throw()`、dtor-跑-finally、`yield from`、一般 Iterator/Traversable foreach | 各独立批 |

## 6. 已知差异（记 PLAN/01 §10，实现后同步）

- **未耗尽生成器销毁不跑 finally**（PHP 会在 dtor 把帧挂回执行 finally）——v1 帧直接销毁；有 finally 的生成器建议显式耗尽或 try/finally 在生成器外
- `throw()` 未实现（v2）
- `yield from` 未实现（v2）
- 一般 Traversable/Iterator 的 foreach 未实现（v1 仅 Generator + 数组）

## 7. 测试清单（v1）

| 测试 | 覆盖 |
|---|---|
| `tests/lang/test-generators-basic.php` | 方法/闭包生成器、foreach、自动键、提前 return、嵌套调用生成器函数 |
| `tests/lang/test-generators-send.php` | send 回传、显式键、auto_key 规则 |
| `tests/lang/test-generators-edge.php` | rewind 重跑错误、getReturn 未返回错误、已关闭 foreach 错误、RUNNING 重入 |
| （负向，加载期） | 函数体外 yield、默认值里 yield、非法返回类型——探针式 EXPECT-EXIT |

## 8. 参考

- php-src 8.5.7：`zend_generators.c/.h`、`zend_compile.c`（zend_compile_yield / mark_function_as_generator / GENERATOR_CREATE 插入）、`zend_vm_def.h`（YIELD:8394 / GENERATOR_CREATE:4649 / GENERATOR_RETURN:4727）
- 语义细节速查：2026-10-23 会话日志「深挖 PHP 生成器语义全集」
- tphp 侧锚点：DO_FCALL 建帧（vm.c）、nested run deep（autoload/thunk 先例）、内部类注册（Closure/Worker 模式）、FE_RESET/FE_FETCH（vm.c:835）
