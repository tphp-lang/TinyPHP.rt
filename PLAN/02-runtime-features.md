# 02 · 运行时功能规格（有哪些功能）

> 上级索引：[../PLAN.md](../PLAN.md)。解释型运行时的三大现代化能力：无感异步、真线程、单二进制；外加 stdlib 与 CLI 的完整 API 面。

## 1. 透明异步 I/O（无色模型，M4）

所有下列函数在协程内自动挂起/恢复，**同步语义 100% 不变**（返回值、错误行为与 PHP 相同）；不在协程内（顶层同步脚本）则内部自动进主协程，行为仍是"等待完成"后继续：

| 类别 | 函数（首版） |
|---|---|
| 时间 | `sleep` `usleep` `time_nanosleep`（uv timer，CPU 0%） |
| 文件 | `file_get_contents` `file_put_contents` `fopen` `fread` `fwrite` `fgets` `fclose` `feof` `fseek` `file_exists` `filesize` `unlink` `rename` `mkdir` `rmdir` `scandir`（uv_fs 线程池） |
| 网络（TCP/UDP） | `fsockopen` `stream_socket_client` `stream_socket_server` + `fread/fwrite` 流式读写（uv_tcp/udp，真异步非线程池） |
| DNS | `gethostbyname` `gethostbynamel`（uv_getaddrinfo） |
| HTTP 客户端 | `http_get(string $url, array $opts = []): string`（新内置；重定向/超时/自定义头，M4 简版；**https 走 mbedtls**） |
| 进程 | `exec` `shell_exec` `proc_open`（M6，uv_spawn） |
| 信号 | `Tphp\on_signal(SIGINT, fn)`（M6+） |

**HTTPS（M4）**：libuv 不提供 TLS → vendor mbedtls + `cacert.pem` 根证书内嵌进 exe（txiki `src/cacert.c` 同款，`scripts/update-ca-bundle.sh` 可直接抄来维护证书）；`http://` 与裸 TCP/UDP 不受影响。**注意**：HTTP/1.1 客户端必须支持 **chunked transfer 解码**（主流响应均为 chunked），否则 https 验收基本必挂；重定向、Content-Length、keep-alive=close 简版即可。

**无感的含义**：`sleep(1)` 期间其他协程照常跑、CPU 占用 0%；`file_get_contents` 阻塞等待期间事件循环照常调度——用户写的是同步代码，得到的是 JS 式并发。

## 2. `Tphp\` 显式 API（M4）

```php
namespace Tphp;

function spawn(callable $fn, mixed ...$args): Coroutine;   // 创建协程，立即返回
function await(Coroutine|Channel $c): mixed;               // 等待结果/透传异常
function all(array $coros): array;                          // 全部完成，按序返回值数组
function any(array $coros): mixed;                          // 任一完成（首个成功或全败）
function suspend(): void;                                   // 主动让出一次
function current(): Coroutine;                              // 当前协程
function run(callable $main): void;                         // 入口：主函数跑完事件循环再退
```

```php
class Coroutine {
    public function cancel(string $reason = ''): void;      // 协作式取消 → 挂起点抛 AsyncCancelled
    public function isRunning(): bool;
    public function isDone(): bool;
    public function getResult(): mixed;                     // 未完成抛错
    public function getException(): ?Throwable;
    public function wait(): mixed;                          // await() 的对象式别名
}

class Channel {
    public function __construct(int $capacity = 0);         // 0=无界
    public function send(mixed $v): void;                   // 满则挂起；closed 抛错
    public function recv(): mixed;                          // 空则挂起；closed+空返回 null
    public function close(): void;
    public function len(): int;
}

class TaskGroup {                                          // Tphp\TaskGroup（协程任务组）
    public function spawn(callable $fn, ...$args): Coroutine;  // 启动并登记
    public function wait(): array;                          // 全部完成，按登记序返回结果
    public function count(): int;
}

class Task {                                               // async($callable) 的返回物
    public function __invoke(...$args): Coroutine;          // 调用即启动
}
```

**语义要点**：
- **`async(EXPR)` 语法结构**：唯一实参是**调用表达式**时，parser 改写为 `Tphp\spawn(fn() => EXPR)`——表达式整体在协程内执行，不会先求值；自由变量按 spawn 那一刻**按值捕获**。单实参非调用形态（`async($callable)`）退化为包装成 `Task`。`$obj->spawn(EXPR)` 同规则（`spawn` 是并发保留方法名，与 `async` 一致）
- **取消**：`cancel()` 后协程在下一个挂起点抛 `AsyncCancelled`（直接继承 Throwable，`catch(Exception)` 不会误吞）；finally 保证资源清理
- **死锁检测**：所有协程挂起且事件循环无任务 → `DeadlockError`（标注各协程挂起位置）
- **无需 Mutex**：协程只在挂起点切换、无抢占，单线程内没有数据竞争——这是无色模型的核心卖点，不提供 Mutex/Synchronized
- **多等待者**：`recv` 空 / `send` 满时的等待者按 **FIFO** 排队唤醒，多消费者与多生产者都支持；`send` 优先直接交接给队首 `recv`（值走协程私有槽），否则入缓冲。因此 Channel 可直接当**信号量**做并发限流（预置 N 个令牌，任务 `recv` 取、`finally` 归还）
- 后置（M7+）：`Tphp\scope()` 结构化并发、`Tphp\select()`、`withTimeout($c, $ms)`

## 3. Worker 线程 API（M5，真 OS 线程）

```php
namespace tphp;

class Worker {
    public function __construct(Closure|string|array $entry, array $opts = []);
    // $entry 三形态（P1-10，与 Thread::spawn 相同，全局解析 main + inc_progs + builtins）：
    //   Closure        —— 入口闭包特例：构造时其 TFunc 字节码只读共享、
    //                     捕获值按下方克隆协议拷贝到子线程（捕获不可序列化
    //                     → NotCloneableError，报错指向改函数名/静态方法形态）
    //   string 'fn'    —— 全局函数名（require 进来的库/vendor 函数照样可作入口）
    //   [Cls, 'method']—— 静态方法（类表每线程克隆，按类名+方法名重建；可触发 autoload）
    // $opts: ['stack_size' => 2*1024*1024, 'name' => 'my-worker']

    public function post(mixed $value): void;               // 投递给子线程（异步，内部克隆）
    public function recv(float $timeout = -1): mixed;       // 接收子线程消息（协程内挂起，不阻塞 loop）
    public function on(string $event, callable $fn): void;  // 'message' | 'error' | 'close'
    public function stop(float $timeout = 5.0): void;       // 优雅停止：通知→join→释放
    public function isRunning(): bool;
    public function id(): int;

    public static function self(): Tphp\Worker\Context;     // 子线程内取自身上下文
}
```

**模型**（txiki Worker 同款）：
- 子线程 = **独立 VM 实例**：独立堆/VM 栈/intern 表/事件循环/协程调度器，**绝不共享 PHP 对象**——没有 Mutex、没有共享内存陷阱，并发安全靠隔离 + 消息，不靠纪律
- 子线程内可继续 `Tphp\spawn`（协程）和再开 Worker（线程树）
- 启动：`uv_thread_create` + `uv_sem` 握手（构造返回时子 runtime 已就绪）；停止：父线程独占生命周期 Stop→uv_async 唤醒→uv_thread_join→Free

**跨线程值克隆规则**：

| 值类型 | 行为 |
|---|---|
| null/bool/int/float/string | 直接深拷贝 |
| array | 递归深拷贝（循环引用检测到即报错） |
| object | 需实现 `__cloneState(): array`（自定义克隆协议），否则抛 `NotCloneableError` |
| closure | **可序列化**（T_CLOSURE 格式 = Program 注册表 id + 函数索引 + 递归捕获，2026-10-26 实现）；资源句柄不可传（抛错） |
| 类/函数定义、lib 字节码、interned 字符串字面量 | **只读共享，零拷贝** |

**背压**：mailbox 有界（默认 1024），`post` 满时协程内挂起（同步顶层则阻塞并告警）。

## 3.1 Thread 真线程池 API（`Tphp\Thread`，2026-10-26）

```php
namespace Tphp;

class Thread {
    public function __construct();
    // 入口三形态（P1-10，对齐 PHP 全局符号表心智）：
    //   Closure            —— 按 Program 索引序列化（捕获必须可序列化）
    //   string 'fn_name'   —— 全局解析（main + inc_progs + builtins）：
    //                         require 进来的库/vendor 函数照样可作入口
    //   [Cls, 'method']    —— 静态方法（按类名+方法名在子线程重建；
    //                         类表每线程克隆，指针不可跨线程）
    public function spawn(Closure|string|array $entry): mixed;  // 起一次性 OS 线程
    public function wait(): array;                // 阻塞等全部结束，按 spawn 序返回结果数组
    public function detach(): void;               // fire-and-forget：立即返回，线程自己跑完
    public function count(): int;                 // 当前跟踪中的线程数
    public function poll(): array;                // 非阻塞收割已完成线程的返回值（不挂起协程）
}
```

**与 TaskGroup（协程组）的分工**：

| | `Tphp\TaskGroup`（协程） | `Tphp\Thread`（真线程） |
|---|---|---|
| 并行度 | 单线程内交错 | 多核真并行 |
| 阻塞调用 | 卡住整个调度器 | 只卡住该线程 |
| 捕获变量 | 同一份内存 | 跨线程**深拷贝**（序列化协议） |
| 适用 | I/O 密集、大量并发 | 阻塞/计算密集；**UI 线程回调里跑长任务** |

**为什么 UI 回调里必须用真线程**：协程是单线程协作式调度，`webview_run` 这类 C 阻塞调用占着唯一的 UI 线程 → 协程永远得不到调度（"点了按钮没反应"）；让 main 主动挂起则 UI 消息泵冻结。真线程不依赖调度器让出，在 FFI/C 回调里 `spawn` 立即开跑。

**实现要点**：
- 每次 `spawn` = 一个一次性 Worker（复用 §3 的 mailbox + 序列化基础设施），入口 = 三形态之一（见上方签名；字符串/数组形态不建 thunk、不捕获 `$this`）
- **子线程继承父线程全局 Program 视图**（P1-10）：入口 owner 作 `vm->prog`，主 + 其余 inc_progs 挂进子线程 `inc_progs`——入口函数跨文件调用、动态按名调用与父线程同一张全局符号表（PHP 单请求符号表的等价物）
- `main` 返回值经 `tphp_sched_run(TVal *main_ret)` 传出 → 子线程 serialize 后经 mailbox 回传父线程
- `poll()` 用非阻塞 `tryRecv`，供 UI 线程安全收割（不挂起协程）
- `detach()` 把 Worker 引用移交进程级孤儿表（父侧不再跟踪、不 join）
- **捕获对象需实现 `__cloneState()`/`__restoreCloneState()`**，否则抛 `NotCloneableError`；闭包可捕获（走 T_CLOSURE 序列化：Program id + 函数索引 + 递归捕获）。入口闭包的捕获在构造时**预检**（worker_cap_ok），不可序列化（FFI CData / 无 `__cloneState` 的对象，典型 = 方法内普通闭包自动捕获的 `$this`）→ 报错并指引改用函数名/`[Cls,'method']`/`static function` 形态
- 方法内定义的普通闭包自动捕获 `$this`（P1-3，PHP 对齐）；spawn 场景请用 `static function`（不绑 `$this`，`self::` 作用域仍生效）或函数名/静态方法形态——pebview `bindAsync` 即此改法

## 4. 标准库清单

**C 原生**（builtin.c，最热 20%）：
- 数组：`count array_key_exists in_array array_push array_pop array_shift array_unshift array_keys array_values array_merge array_slice array_reverse array_sum sort rsort`
- 字符串：`strlen substr str_replace strpos stripos strrpos trim ltrim rtrim strtolower strtoupper str_repeat str_pad(简) implode explode str_split strrev number_format(简) sprintf(常用 %[sd.fx])`
- 数学：`abs min max intdiv round floor ceil sqrt pow(简) mt_rand`
- 变量：`isset empty unset is_int is_string is_array is_float is_null is_bool gettype intval floatval strval boolval var_dump print_r`（**无 compact**：按名字读局部变量表需要动态作用域机器，与静态槽位 VM 冲突，同 `$$var`/`extract()` 禁用理由，见 PLAN/01 §9）
- 序列化：`serialize unserialize`（复用跨线程克隆协议 serialize.c，M5；数组/标量/带 `__cloneState()` 的对象，自有格式，**与 PHP serialize 文本不互通**——与 PHP 交换数据仍用 JSON）
- JSON：`json_encode json_decode`（C 原生手写，M6）
- 杂项：`str_contains str_starts_with str_ends_with`、`array_map/filter/reduce/usort`（接受 callable）

**PHP 层**（lib\，打包进二进制）：`sprintf` 全量、`date()/mktime()/checkdate()`、`array_column array_combine array_flip array_walk array_chunk array_splice`、`strtr ucwords nl2br strip_tags`、`Tphp 高层组合器`、后续 `Str/Arr` 工具类

**后置**（M7+）：pcre2 vendor 后 `preg_*` 全套、内置 SQLite（libsqlite3 vendor，`Sqlite\Db` 对象 + 协程化）、HTTP server（`Http\Server`）、REPL

## 4.5 FFI（M5.5 ✅ 已完成，txiki mod_ffi 路线）

**定位三层，与单二进制正交**：
1. **系统库绑定**：user32/kernel32/ws2_32/msvcrt（Linux libc/libm）每台机器自带 → `FFI::cdef('...', 'user32.dll')` 零配置天然兼容单 exe
2. **要内嵌的库走 vendor 静态链接**（项目先例 mbedtls/cacert/libuv），做成内部类而非让用户 FFI
3. **第三方 DLL**：exe+dll 同发即可；可选 `tphp build --with` 打包器（见 PLAN/03 §6.1——数据资产内存直读，`.dll/.so/.dylib` 按等效路径提取到 exe 同级）

- **API**：`FFI::cdef($code, $lib = null)` / `FFI::new($type[, bool owned = true])` / `FFI::memcpy($dst, $src, $n)` / `FFI::free($ptr)` / `FFI::sizeof`；`FFI\CData` 对象（属性/下标读写、作为调用参数/返回值）
- **cdef 子集**：函数声明、基本类型、指针、平铺 struct/enum、简单 typedef；**拒预处理指令**（#include/#define 不解析——cdef 字符串里出现即 ValueError）；不带完整类型系统与 preload（php-src 全套 bison 解析器 ~1.1 万行，过重不学）
- **实现**：libffi vendor 进 deps（txiki mod_ffi 同款，1623 行先例）+ LoadLibrary/GetProcAddress（Windows）/ dlopen（Linux/macOS 后置）
- **内存策略**：FFI::new 记 owned 标志，CData 对象被 GC 回收时若 owned 则释放原生内存；FFI::free 显式提前释放；非 owned（取自 struct 字段/返回值的视图）不重复释放
- **与并发**：FFI 调用是阻塞 native 调用（不挂起调度器、事件循环停摆）——CPU 密集/长阻塞调用放 Worker 线程（M5），文档示例承载此约定
- **参考**：txiki.js src/mod_ffi.c（libffi + Pointer/External）、php-src ext/ffi（API 语义对齐目标，实现不照搬）

### 4.5.1 FFI 回调（C 函数指针 → PHP 闭包；v1 设计定稿 2026-10-22）

四家对照（php-src 8.5.7 / txiki / Bun / V 0.5.2，详见会话分析）后的取舍：**API 形态与生命周期抄 PHP，异常抄 PHP 语义但用干净 exit 实现，编组抄 PHP 但修返回值垃圾坑，线程走 Worker 路线，地址面比三家严**。

- **API**：零新类型。cdef 参数位置支持函数指针声明 `int (*compar)(const void *, const void *)`；闭包/可调用传给函数指针参数时**隐式**建 libffi trampoline（`ffi_closure_alloc` + `ffi_prep_closure_loc`）。v1 仅参数位置；struct 字段/数组元素赋值形态 v2 后置
- **生命周期**：trampoline + 闭包（addref）进入**线程局部回调表**，脚本/线程结束释放——CLI 单脚本 ≈ PHP 单请求"整请求有效"；无显式 close（学 PHP 非 Bun）；Worker 每线程独立表
- **编组（C→PHP 参数）**：标量直转；`const char*` → 拷贝 TStr；其他指针 → 非 owned CData 视图；struct 参数 → 非 owned CData 别名 libffi 栈（回调返回后失效，文档红字）；varargs 回调与 array 参数**不支持**（对齐 PHP）
- **编组（PHP→C 返回）**：标量宽松转换 + 同尺寸 CData memcpy；**类型不匹配 → stderr 警告 + 返回类型零值**（修 PHP 的"吞异常返回垃圾"坑）
- **异常**：v1 曾"回调内 throw = 硬禁止"——**已被 v2③ 可捕获设计取代**（见下）；历史：v1 实现 = raise 钩子查 `tphp_ffi_cb_depth` >0 → 打印 + exit（比 PHP bailout 干净）。回调内允许嵌套 ffi_call（重入）
- **线程**：同线程同步；C 跨线程调回调 = 未定义（文档禁止）；不做 Bun 式 threadsafe 投递——Worker 隔离是官方答案
- **地址面**：trampoline 不暴露裸地址（传参只认 Closure 值），配合既有 `cast` 拒裸 int
- **验收**：`tests/ffi/test-ffi-callback.php` —— msvcrt/libc `qsort` + 捕获闭包端到端
- **v1 已完成**（2026-10-22，184/184）：FFI_FUNC_PTR 真解析 + trampoline + TLS 保活 + `vm_unwind_jump` 全量收口（op_throw 曾直接 longjmp 绕过 depth 检查——所有 unwind 出口必须经它）
- **v2 已完成**（2026-10-22，185/185）：
  1. **struct 字段/数组元素的闭包赋值**——写路径走既有 encode_arg（FUNC_PTR→closure_create）；补 fetch_prop/assign_dim 读回为非 owned 函数指针视图；`FFI::new("struct S")` 静态调用（sc=NULL）由 AV 改为明确 ValueError（结构体类型需 `$ffi->new`）
  2. **同闭包复用去重**——closure_create 按「T_CLOSURE 对象指针 + 签名 FFIType*」查 TLS 表复用同一 trampoline；失败路径（prep_cif/alloc/prep_loc）raise 前释放 fc/ats/closure（不学 PHP 静默泄漏）
  3. **异常语义修正 + 可捕获设计（v2③，已定稿待实现）**——v1 的"回调内 throw = 硬 Fatal"参照错了对象：php-src trampoline 对普通 throw 的真实行为是 `zend_call_function` 返回后查 `EG(exception)` → `E_WARNING` + `zend_clear_exception()` + 返回零值，**进程不死、回调内 try/catch 正常工作**（PHP 异常是 EG(exception) 检查点模型非 longjmp；Fatal 只属 bailout）。tphp 的困境是 raise=longjmp、unwind 穿 trampoline C 栈帧是 UB，v1 图省事 exit。**设计（2026-10-27 定稿，源自 pebview bind 崩溃复盘）**：trampoline 级 setjmp + 有界 unwind——
     - `cb_trampoline`：保存外层 `vm->x.unwind` → 换成 local setjmp → 跑 `tphp_vm_call_callable`；raise 落到 local（不跳 fiber root、不穿 C 栈）
     - `vm_unwind_bounded(vm, floor=entry)`：现有 `vm_unwind` 加 floor 帧参数——catch 命中恢复执行（再跑到 entry 弹出）；剥到 floor 即停报 escaped（不弹 trampoline 调用方帧）
     - escaped → stderr warning（含 class/msg/file:line，`ex->file` 已有）+ 返回类型零值（复用 cb_encode_ret 宽松语义）→ `vm->x.unwind` 恢复外层 → depth--
     - `vm_unwind_jump` 的 `depth>0 → Fatal` 降级为兜底断言（raise 已被 local setjmp 接管，正常不可达；仅 trampoline C 初始化段异常才走）
     - **三层保证**：①回调内 try/catch 完全正常（C 无感知）②未捕获 = warning + 零值、进程不死（对齐 PHP E_WARNING）③结构性损坏兜底 Fatal
     - **边界**：重入天然嵌套（每层 trampoline 各有 local jmp_buf）；qsort 比较器未捕获 throw → 每次比较 warning+0（比较相等，顺序可能不对——与 PHP 一致，文档注明回调必须自己 catch）；逃逸不 re-raise 到 FFI 调用点外（PHP 也不传播；真要做是 TLS pending 槽 + 边界检查，语义模糊：qsort 调 5 次比较器传哪个——超出 PHP 的增强后置）
     - 实现量：cb_trampoline 重写 ~80 行 + vm_unwind_bounded ~40 行 + 测试 4 条（回调内 catch / 未捕获 warning+零值+进程活 / 嵌套重入 / qsort 抛异常不崩）
     - 动机案例：pebview bind 里 `Thread::spawn` 序列化失败（Window 缺 __cloneState）→ 硬 Fatal 杀进程；可捕获设计下变 JS Promise error payload
     - **v2③ 已完成（2026-10-27，223/223）**：`tphp_vm_call_bounded`（vm.c：local setjmp + `vm_unwind_core(floor)` 有界弹帧；逃逸恢复外层 unwind 返回 false）+ trampoline 打 E_WARNING 风格警告并零值返回；`vm_unwind_jump` 的 cb_depth Fatal 已移除（local pad 接管）。坑：floor 逃逸分支不得重置 n_calls/n_args（会抹掉 C 栈仍压着的 FFI call rec → 0xC0000005）；回归 `tests/ffi/test-ffi-callback-throw.php`
- 验收测试：`tests/ffi/test-ffi-callback.php`（v1 qsort）+ `tests/ffi/test-ffi-callback-struct.php`（v2 字段赋值/读回/去重同地址/静态 new 抛错/读回指针传 qsort/200 次循环平稳）

## 5. CLI

```
tphp run <file.php> [args...]     运行（默认子命令可省）
tphp -e '<php code>'              直接执行代码串
tphp --dump-opcodes <file>        反汇编输出（调试必备）
tphp -v / --version               版本 + 编译目标三元组
tphp --uv-version                 libuv 版本（M0 自检）
tphp test <dir>                   测试运行器（M6，EXPECT 断言协议）
```

## 6. 功能验收示例

**异步示例（M4 验收）**：
```php
<?php
use function Tphp\{spawn, await, all};

// 1) 无感：sleep 不占 CPU，不阻塞其他协程
spawn(function () {
    sleep(1);                                  // 透明挂起
    echo "woken after 1s\n";
});

// 2) 并发 I/O：总耗时 ≈ max(单个)，而非总和
$t0 = microtime(true);
[$a, $b] = all([
    spawn(fn() => file_get_contents('https://api1.example.com/x')),
    spawn(fn() => file_get_contents('https://api2.example.com/y')),
]);
echo "elapsed = " . round(microtime(true) - $t0, 2) . "s\n";

// 3) Channel：生产者/消费者
$ch = new Tphp\Channel(4);
spawn(function () use ($ch) {
    foreach (range(1, 5) as $i) $ch->send($i * $i);
    $ch->close();
});
while (null !== ($n = $ch->recv())) {
    echo "got $n\n";
}

// 4) 取消
$task = spawn(fn() => sleep(10));
$task->cancel('no longer needed');
try {
    await($task);
} catch (AsyncCancelled $e) {
    echo "cancelled: {$e->getMessage()}\n";
}
```

**线程示例（M5 验收）**：
```php
<?php
use Tphp\Worker;

// 父线程：开 4 个 worker 跑 CPU 密集任务，主线程事件循环不受影响
$workers = [];
for ($i = 0; $i < 4; $i++) {
    $w = new Worker(function () {
        $me = Worker::self();
        while ($job = $me->recv()) {           // 空则挂起在本线程 loop 上
            $me->post(sha256_benchmark($job)); // CPU 密集随便跑满一核
        }
    });
    $w->on('message', function ($result) use ($w) {
        echo "[worker {$w->id()}] $result\n";
    });
    $workers[] = $w;
}
foreach ($workers as $w) $w->post('job-data');
// 主线程此时 sleep/file_get_contents 照常并发（worker 不阻塞 loop）
sleep(2);
foreach ($workers as $w) $w->stop();
```

**单二进制示例（M6 验收）**：
```
build\tphp.exe examples\hello.php      → 正常输出，无任何外部依赖
build\tphp.exe -e "echo json_encode(['ok'=>true,'n'=>42]);"
  → {"ok":true,"n":42}                 （json/sprintf/date 已编入 exe）
```

**不支持特性的报错示例**（`CompileError` 家族在**加载期**报出——脚本尚未执行任何一行，对齐 PHP 7+ 的 `ParseError/CompileError` 行为；`DivisionByZeroError` 等则是 opcode 执行时的**运行期**报错）：
```
>>> $a = 1; $b = &$a;
CompileError: references (&$var) are not supported at line 1

>>> echo @file_get_contents('x');
CompileError: error suppression "@" is not supported; use try/catch at line 1

>>> echo $$name;
CompileError: variable variables are not supported at line 1

>>> echo $undefined + 1;
CompileError: possibly-undefined variable $undefined (strict mode) at line 1

>>> intdiv(1, 0);         // 运行期
DivisionByZeroError: Division by zero（对齐 PHP 8）
```
