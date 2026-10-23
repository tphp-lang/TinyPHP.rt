# 04 · 里程碑、测试与风险

> 上级索引：[../PLAN.md](../PLAN.md)。每个里程碑绑定明确的语法范围 + 功能范围 + 可执行的验收标准。

## 1. 里程碑

| 里程碑 | 语法范围 | 功能范围 | 验收标准 | 预估行数 |
|---|---|---|---|---|
| **M0 骨架**(0.5周) | — | 构建脚本 + vendor libuv/mimalloc + 抄 asm + CLI | `build.cmd && build\tphp.exe -v` 输出版本；--uv-version 正常 | ~250 |
| **M1 值+表达式**(1.5周) | 词法全量、过程式语句、数组全语义（键规范化冻结，含 `list()`）、`global` 别名读写、运算符全量（`|>` 管道随 M2，依赖函数调用）、插值 | VM + GC(标记-清扫) + var_dump | tests/lang 全绿：fib(30)、数组键专测、== 矩阵、优先级 30+ 用例 | ~3500 |
| **M2 函数+异常**(1周) | 函数/闭包/箭头函数/命名参数/...、`|>` 管道（PHP 8.5）、heredoc、异常全体系 | builtin 首批、--dump-opcodes | 闭包捕获+递归+调用点缓存；finally 顺序+异常栈打印 | ~2200 |
| **M3 OOP 完整**(1.5周) | PLAN/01 §11 OOP 示例全量可跑 + 命名空间全套（PLAN/01 §8：`namespace`/`use`(as)/`use function`/`use const`、限定名解析对齐 PHP） | class/iface/trait/enum/静态/构造器提升 + spl_autoload_register | 接口多实现+trait 冲突 as+enum match+静态晚期绑定 全部输出预期；命名空间限定名解析专测 | ~2500 |
| **M4 协程+无感I/O**(1.5周) | — | 透明 I/O 全表 + HTTPS(mbedtls+cacert 内嵌) + Tphp\ API | 02 §6 异步示例全输出正确；3 并发 fetch 耗时≈max；sleep CPU 0%；死锁检测 | ~1800 |
| **M5 Worker**(1周) | — | Worker API + 克隆协议 + serialize/unserialize | 02 §6 线程示例：4 worker 满载时主线程 IO 正常；ping-pong 1000 消息有序；stop 退出码 0 | ~1200 |
| **M5.5 FFI**(0.5~1周) ✅ | — | FFI::cdef（C 声明子集解析）+ libffi 调用 + FFI\CData（new/memcpy/free）；DLL 打包器为后置可选项（PLAN/03 §6.1） | 系统库绑定零配置可用（user32/msvcrt）；struct 传参/返回；FFI\CData 读写与 owned 释放；阻塞性调用走 Worker 的文档示例（02 §4.5） | ~1600 已完成 |
| **M6 打包+stdlib**(1周) ✅ | — | stdlib 全量 + phpc + test runner | 02 §6 打包示例；单 exe 零依赖；json/sprintf/date 正确；tests/run.cmd 全绿 | 已完成：JSON/date/require/phpc/tphp-test + m6-acceptance |

总计约 **1.3 万行 C + 800 行 PHP**，单人 6~8 周。每里程碑 `tests\run.cmd` 全绿后进下一阶段，验收 CLI 实际输出贴给用户确认。

## 2. 测试策略

- **txiki 惯例**：一例一文件 `tests\**\test-*.php`，文件内 `// EXPECT: <值>` 注释断言，run.cmd 遍历执行比对 stdout
- 目录分层：
  - `tests/lang/`：数组键专测（"08"/"8.5"/负数键/键混用）、`==` 宽松矩阵、优先级 30+ 用例、插值、数字语义
  - `tests/oop/`：trait 冲突矩阵（as/insteadof）、enum、接口、静态晚期绑定
  - `tests/async/`：并发时序断言、取消传播、Channel 背压、死锁检测
  - `tests/worker/`：消息序、克隆协议、退出协议、嵌套 worker
  - `tests/ffi/`：系统库绑定（GetTickCount/msvcrt 数学）、struct 传参/返回、CData 读写与 owned 释放、cdef 拒绝预处理指令
  - `tests/lib/`：json/sprintf/date 对拍（与 php-src 输出逐例比对）
  - `tests/bench/`：fib(28)、10^6 push、读改写 10^5 —— 结果 EXPECT 恒定，耗时 `error_log` 走 stderr 做回归基线 ✅
  - `tests/php-compat/`：**php-src 对拍层**——`tools/convert-phpt.ps1` 抽 `Zend/tests` 纯 EXPECT 子集转 `test-*.php`；种子 17 例已入库（match/enum/anon/return_types），失败用例丢弃待能力补齐后重转（见 tests/php-compat/README.md）
- `--dump-opcodes` 反汇编输出（compile.c 顺带实现），Bug 定位必备
- 失败诊断流程：跑单测 → --dump-opcodes 看编译产物 → 一次只修一个问题（用户调试习惯）

## 3. 风险与规避

| # | 风险 | 规避 |
|---|---|---|
| 1 | Windows Fiber 栈切换 / CET Shadow Stack | 照抄 `Zend/asm/make,jump_x86_64_ms_pe_gas.S`（php-src zend_fibers.c:440-451 实证 shadow stack 指针传递），**不自写汇编**；ASAN 与 fiber 不兼容，asan 构建走非 fiber 退化路径（mimalloc 同样与 ASAN 互斥——txiki BUILD_WITH_MIMALLOC 先例，ASAN 构建一并关掉） |
| 2 | 循环引用内存泄漏 | M1 就内置简易标记-清扫 GC（根集 = VM 帧 + 全局表 + **挂起协程链表**（含 fcontext 切走的 C 栈上暂存的 TVal）——M1 预留该根集槽位，M4 直接接入，不留到协程做完才返工） |
| 3 | 递归下降优先级坑（`??` vs `?:` vs `=`） | 优先级表驱动而非手写嵌套，配 prec.php 30+ 用例 |
| 4 | `==` 宽松语义矩阵大且易错 | 直接对照移植 php-src `zend_operators.c` 的 compare_function（唯一整段借鉴），tests/lang 逐例对拍 |
| 5 | uv threadpool（默认 4）饱和 | 文件 I/O 走线程池、网络走真异步；Worker 内嵌套 I/O 限流；Channel 背压写文档+测试 |
| 6 | libuv vendor 方式 | 直接拷源码入库（txiki 同款，不用 submodule）；build.cmd 显式列 `deps\libuv\src\win\*.c` |
| 7 | `.cmd` 非 ASCII 串行 | 构建脚本纯 ASCII（用户环境已知约束） |
| 8 | 里程碑膨胀 | 每阶段绑定明确验收命令；不做验收外功能（用户"不为修而修"原则） |
| 9 | 协程栈内存成本 | fcontext 栈默认 2MB/协程，批量 `spawn`（如成千超时任务）会爆内存：栈池复用 + 小栈默认（如 64KB 起步按需扩），`stack_size` 可调；M4 设计时定死策略 |
| 10 | 死锁判定与正常退出混淆 | 所有 uv handle 均 `uv_unref`，`uv_run(UV_RUN_DEFAULT)` 无活动 handle 时返回 0——此时须区分"事件循环正常耗尽"（无挂起协程）与"全体协程挂起"（`DeadlockError`）；判据：返回 0 且挂起协程链表非空即死锁，写进 sched.c 设计 |
| 11 | HTTPS 依赖 mbedtls（体积/配置/证书过期） | vendor 固定版本；`MBEDTLS_CONFIG_FILE` 裁剪到 TLS1.2/1.3 客户端最小集（体积增幅 <400KB）；cacert.pem 内嵌并随依赖版本更新（txiki cacert.c 先例） |

## 4. 执行顺序

M0 → M1 → M2 → M3 → M4 → M5 → M5.5 → M6 串行推进。

每阶段结束动作：① `tests\run.cmd` 全绿 ② 跑验收示例 ③ 把实际 CLI 输出展示给用户 ④ 用户确认后进下一阶段。
