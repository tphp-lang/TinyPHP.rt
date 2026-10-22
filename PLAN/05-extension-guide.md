# 05 · 内置扩展开发指南（贡献者规范）

> 上级索引：[../PLAN.md](../PLAN.md)。面向后续加入的开发者（人类或 AI）：如何给本项目添加新的内置能力、必须遵守哪些红线、按什么流程交付。
> 本项目所有"扩展"都是**内置**的（编译进 exe），不存在动态加载的 Zend 扩展——PHP 原生扩展 DLL 与本项目 ABI 不兼容（扩展是 php8.dll 的插件，直接解引用 zval/HashTable 内存布局），结论与替代路径见项目记忆/PLAN/02 §4.5。

## 1. 新能力选型（先问路径，再动手）

| 路径 | 适用 | 先例 |
|---|---|---|
| **纯 PHP（lib\*.php）** | 能用现有 builtin/内部类表达的；纯算法/组合逻辑 | date/array_column（M6 打包进 exe） |
| **builtin 函数（C）** | 单个无状态操作，参数→返回值，不需要对象 | strlen/sort/serialize |
| **内部类（C）** | 有状态（句柄/上下文）、需要方法调用/生命周期管理 | Async\Channel、tphp\Worker、TStream |

判断标准：需要持有 OS 资源（fd/socket/timer/线程）或跨调用状态 → 内部类；一次调用完成 → builtin；C 标准库没有现成实现且纯 PHP 性能可接受 → lib。**禁止**为了"快"把纯逻辑下沉到 C（先测性能，bench 有证据再说）。

### 1.1 目录布局（`src/` vs `ext/`）

- **`src/` = 引擎**：lex/parse/compile/vm/sched/fiber/value/class/exclass/mem/error/bundle/pack/main——解释器本体与基础设施
- **`ext/` = 内置扩展层**：一扩展一子目录，PHP 可见能力都落这里——
  `ext/builtin/`（无状态函数大表）、`ext/ffi/`、`ext/net/`（含 cacert.c）、`ext/io/`、`ext/worker/`、`ext/json/`、`ext/serialize/`、`ext/pkg/`
- 新扩展**建子目录**（`ext/<name>/*.c|h`），不要往 `src/` 平铺，也不要把新函数继续堆进 `builtin.c` 大表（达到拆分阈值时按域拆 `ext/builtin/`）
- 构建：`build.cmd` / `build.sh` 自动扫 `src/*.c` + `ext/*/*.c`，无需登记；头文件经 `-Iext/<name>` 引用（**-I 列表在 deps/libffi/include 之后**，且子目录内头文件禁止叫 `ffi.h`/`io.h`——会顶掉系统头，见 §4.10）

## 2. builtin 函数开发（`ext/builtin/builtin.c` 模式）

```c
static TVal bi_myfunc(VM *vm, TVal *args, int n) {
    if (n < 1 || args[0].t != T_STR)                    /* 1. 先校验，后使用 */
        RAISE(vm, "TypeError", "myfunc() expects string");
    ...                                                  /* 2. args 只借用 */
    return TV_INT(42);                                   /* 3. 返回值转移所有权 */
}
/* 注册：tphp_builtins[] 表加一行 { "myfunc", bi_myfunc } */
```

- **args 所有权归调用方**：DO_FCALL 调用后统一 release——builtin 内部对 refcounted 参数只读；要长期保存必须 `tval_addref`；返回值所有权转移给调用方
- 参数个数/类型错误 → `ArgumentCountError`/`TypeError`（RAISE 宏）；运行条件不满足按 PHP 语义 Warning(stderr) + false，而不是抛异常（对齐 PHP 8，见 §5）

## 3. 内部类开发（`ext/worker/worker.c` 模式）

```c
/* 1. 建类：属性 + 方法（方法名必须 tstr_intern，防 GC） */
static void myext_make_class(void) {
    TClass *c = worker_make_class("tphp\\MyThing");     /* 类名也 intern */
    myext_add_native(c, "open",  (void *)nm_open,  false);
    myext_add_native(c, "read",  (void *)nm_read,  false);
    myext_add_native(c, "count", (void *)nm_count, true);   /* 静态 native */
    tphp_class_register(c);                              /* 仅注册进本线程 */
}

/* 2. native 方法签名（receiver 引用由 DO_FCALL 归还，内部不得再释放） */
static TVal nm_read(VM *vm, void *self, TVal *args, int n) { ... }
```

- **上下文存取**：轻量状态放对象属性槽（`__handle` 下标模式，见 TStream/Async\Coroutine）；OS 句柄等重资源放进程级注册表 + id（WHandle 模式），对象只存 id——**对象被 GC ≠ 资源被释放**，必须有显式 close 或注册表回收策略
- **注册时机**：内部类必须在用户脚本**编译前**注册（`A_NEW` 有编译期类存在性检查）——main.c 里 `tphp_async_init()` 之前/同处挂你的 init，幂等
- **静态 native**：`is_static=true` 走 op_init_smcall 拦截，DO_FCALL 收到 NULL receiver（Worker::self 先例）
- 对象比较/序列化/GC 遍历：新增对象 kind 时同步改 `tval_compare`/`tval_clone_deep`/`gc_children`（漏一处就是隐形 bug）

## 4. 核心约定红线（违反 = 崩溃或隐性内存错误，历史教训全部踩过）

1. **内存**：统一 `tm_malloc/tm_free`（mimalloc 开关切换）；堆对象必须 `gc_register`；禁止裸 malloc/free 混用
2. **TLS**：模块级可变状态一律 `_Thread_local`（M5 后每线程独立 runtime）——**新增全局变量 checklist：可变？→ TLS**；排查手段：打印两线程 `&变量` 地址相同 = 漏修饰
3. **args 区**：任何"读 args 区"的 opcode/native 路径必须消费完回退 `n_args`（漏回退 → 参数污染下一个调用，`serialize(new Point)` 收 3 参教训）
4. **排队即 addref**：向 args 队列/数组存放 TVal 值时 args 区持有自己的引用（args_push 语义）；variadic 的多余实参归 RECV_VARIADIC，禁止提前 release
5. **挂起协议**：挂起用 SchedEvent 标准三件套（reset/wait/fire 配对），手写挂起必须登记 `ev->waiter`（漏登记 = 永挂）；挂起期间 C 栈冻结，uv 回调只写数据不跑 VM
6. **uv 生命周期**：`uv_close` 是异步的——handle 在 close_cb 里释放，禁止 close 后立即 free（UAF 教训）
7. **异常安全**：`vm_raise` 是 longjmp——native 中途 raise 会跳过后续 C 代码：资源先登记后使用、已分配内存在 raise 前释放或入注册表；挂起型 native 抛出路径由 cont_check_cancel 统一弹 rec（不要自己处理）
8. **跨线程**：Worker 模型无共享——只读共享限 prog 冻结数据（ic 已急解析）；可变数据一律走消息（serialize 克隆）；新全局缓存默认禁止跨线程
9. **阻塞**：native 里禁止自旋等事件（挂死全调度器）；CPU 密集/长阻塞操作 → Worker 线程，或查 uv 异步 API
10. **CRT 头名**：自定义头避开 CRT 系统头名（`io.h`→`tio.h` 教训——-I 优先级顶掉 CRT/依赖声明，clang 新版按硬错误处理）；`ext/` 各子目录的 -I 一律排在 `deps/libffi/include` 之后，且 `ext/ffi/` 内头文件禁叫 `ffi.h`（顶掉 libffi 公共头）

## 5. 错误处理标准（对齐 PHP 8 语义）

| 场景 | 行为 |
|---|---|
| 参数个数不符 | `ArgumentCountError` |
| 参数类型不符 | `TypeError` |
| 值越界/格式非法（fopen 模式、非法枚举值） | `ValueError` |
| 运行条件不满足（文件不存在、连接失败） | **PHP Warning（stderr）+ 返回 false**，不抛异常 |
| 内部不变量破坏（runtime 自身 bug） | `tphp_fatal_src`（不可恢复，exit 255） |
| 新异常类 | 入 tphp_ex_classes 表 + EX_* 常量，继承 Error/Exception 合理层级 |

消息文案对齐 PHP 官方格式（`func() expects exactly 1 argument`），fatal 报告走 stderr（EXPECT 协议只对比 stdout）。

## 6. 安全红线清单

- **输入校验先行**：所有外部输入（脚本传参、文件内容、网络数据）先类型/长度/范围校验再使用；长度用显式 len 字段，**不假定 NUL 结尾**（interned 串/片段切片无 NUL——用 lex_init_len 类 API）
- **memcpy/边界**：长度计算显式逐项核对（历史 bug 全是 off-by-one：`::$prop` 名字构造 n+1 越界写）；数组访问先查容量
- **路径安全**（io/打包器）：外部提供的相对路径归一化后必须仍在受控根内——拒绝 `..`、绝对路径、盘符（zip-slip 防护）
- **原生库边界**（M5.5 FFI 后）：外部指针不可信——FFI\CData 只允许经 cdef 声明的类型访问；资源句柄生命周期归 runtime 注册表；FFI 调用阻塞约定写进文档
- **资源 limits**：新能力考虑耗尽场景（fd 上限、内存上限、递归深度）——给错误路径而不是无限挂起/OOM
- **密码学/随机**：不自己发明；用 mbedtls vendored 或系统 API

## 7. 开发工作流（人机共用）

```
① 写测试先行    tests/<目录>/test-<name>.php，EXPECT 逐行断言（含失败路径）
② 实现          遵守 §4 红线；注释中文；命名 tphp_/bi_/nm_/T 大写结构体
③ 构建          build.cmd 零警告（-Wall -Wextra 顶置）
④ 回归          tests\run.ps1 全绿（新增测试计入）
⑤ ASAN          asan 扫描通过（mimalloc 与 ASAN 互斥，构建命令见项目记忆）
⑥ 验收          验收 CLI 实际输出贴给用户确认
⑦ 收尾          更新 PLAN 对应章节 + 项目记忆（bug 教训必记）
```

- 提交粒度：每个子阶段一提交，信息格式 `M<阶段>-<子阶段>: <做了什么>`（如 `M5-d: 修复 call_callable 预搬参数与 RECV 冲突`）
- 调试工具链：`TPHP_TRACE=1`（VM 执行轨迹）、`--dump-opcodes`（编译产物）、ASAN（内存 bug 第一利器）
- 临时探针放 `C:\Users\28249\AppData\Local\Temp\`（不入库，用户清理）；调试打印用 getenv 开关（TPHP_XXX_DEBUG），定位后删除
- Windows 陷阱：.ps1 生成类脚本注释全 ASCII（PS5 读无 BOM UTF-8 中文吞换行）；test runner Get-Content 必须 -Encoding UTF8；崩溃进程锁 exe → 改名链接（tphp2.exe）绕过

## 8. 上线检查清单（每个新扩展过一遍）

- [ ] 选型正确（lib/builtin/内部类），无过度下沉
- [ ] args 校验 + 所有权语义正确（借用/转移分清）
- [ ] 类名/方法名 tstr_intern；注册先于编译；幂等 init
- [ ] 新全局变量全部 _Thread_local（或证明只读）
- [ ] uv handle 走 close_cb 释放；SchedEvent 三件套配对
- [ ] raise 路径资源不泄漏（ASAN 跑异常场景）
- [ ] 错误语义对齐 PHP 8（§5 表）
- [ ] 安全红线逐条过（§6）
- [ ] EXPECT 测试含成功+失败路径；run.ps1 + ASAN 全绿
- [ ] PLAN/项目记忆已更新
