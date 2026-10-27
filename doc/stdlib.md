# 内置功能与函数清单

当前已实现的内置能力。语法范围见 [../PLAN/01-syntax-spec.md](../PLAN/01-syntax-spec.md)。

## 语言特性

### 类型系统

运行时值类型（`TVal` 标签）：

| 类型 | 说明 |
|---|---|
| `null` | |
| `bool` | `true` / `false`（内部 T_TRUE/T_FALSE） |
| `int` | **int64**；溢出自动转 float（对齐 PHP） |
| `float` | **float64** |
| `string` | 不可变、NUL 结尾（便于 C 互操作） |
| `array` | 有序哈希（list/dict 双语义）；键规范化：`"8"`→int 8，`"08"`/`"8.5"`/超 int64 留字符串 |
| `object` | 用户类实例 / 异常对象 |
| `closure` | 用户闭包 + builtin 一等包装 |
| `enum case` | 单元/带值枚举 |
| resource | **无独立 resource 类型**；文件/套接字句柄封装为对象（如 TStream） |

类型**声明**（编译期解析 + **运行期校验**，非 AOT 约束）：

- 标量：`int` `float` `string` `bool` `void`
- 可空：`?int`（等价 `int|null`）
- 联合：`int|string`、`array|null` 等
- 容器：`array` `iterable` `mixed`
- OOP：`static` `self` `parent`、类名、`enum` 名
- **不支持**（后置）：交叉类型 `A&B`、DNF `(A&B)|C`

动态特性全部保留：`$fn()`、`new $cls`、`$obj->$m()`、`$obj->{'prop'}` 等——类型只在调用/赋值点校验。

### 语法与语义

- 过程式 + 完整 OOP：`class` / `interface` / `trait` / `enum`（含 backing value、`match`）
- 命名空间：`namespace`、`use` / `use as` / `use function` / `use const`、**组 use** `use A\B\{C, D as E}`
- 函数：顶层/嵌套、闭包 `function() use`、箭头 `fn()`、命名参数、变参 `...`、默认值、first-class callable `$f(...)`
- 控制流：`if/while/do/for/foreach/switch/match`、`break N` / `continue N`、`goto`（受限）
- 异常：`try/catch/finally/throw`，内置异常层级（`Throwable` → `Error`/`Exception` → …）
- 数组：`[]`、自动追加、键规范化（`"8"`→int 8，`"08"` 留字符串）、解构、展开、`list()`
- 管道：`|>`（PHP 8.5），编译期展开
- `global` 读写、`static` 局部静态、`require`/`include`（字符串字面量，编译期内联）
- 类型声明：参数/返回/属性（运行期校验）：`int` `?int` `int|string` `array` `mixed` `iterable` `static` 等
- heredoc / nowdoc、双引号插值、`<?php` / `<?=` / 内联 HTML

**不支持**：`&$ref` 引用传值、`extract`/`compact`/`$$var` 可变变量、`@` 错误抑制、`foreach &$v` 引用、交叉类型/DNF（后置）。

### 内置常量

| 常量 | 值 |
|---|---|
| `PHP_EOL` | 行尾符（Windows `"\r\n"`，其余 `"\n"`） |
| `DIRECTORY_SEPARATOR` | 路径分隔符（Windows `"\"`，其余 `"/"`） |
| `PATH_SEPARATOR` | PATH 分隔符（Windows `";"`，其余 `":"`） |
| `PHP_OS_FAMILY` | 系统家族：`"Windows"` / `"Linux"` / `"Darwin"` / `"BSD"`（对齐 PHP，macOS 归 Darwin） |
| `PHP_VERSION` | 运行时版本号（如 `"0.0.2"`，来自 `TPHP_VERSION`） |
| `PHP_PACK` | **tphp 扩展**：是否运行在打包 exe 内（bool；`tphp run`/源码执行恒 false） |
| `PHP_UNAME` | 关联数组，uname(2) 五字段：`sysname` `nodename` `release` `version` `machine`（libuv `uv_os_uname`，Windows 同样提供；替代不规范的 `php_uname()` 全局函数） |
| `PHP_INT_MAX` / `PHP_INT_MIN` | int64 边界 |
| `FILE_APPEND` / `LOCK_EX` | `file_put_contents` 选项位 |
| `SEEK_SET` / `SEEK_CUR` / `SEEK_END` | `fseek` whence |
| `ASSET_ROOT` | `"tphp://asset"`——打包应用资源根（io 层直读内存 zip；未打包时读取告警） |

`PHP_PACK` 配合 `ASSET_ROOT` 的典型用法：开发期读磁盘、打包后读内存 zip，同一份代码两个运行形态。

## 内置函数（C 原生，builtin.c）

### 输出与调试
`print` `var_dump` `print_r` `echo`（语言结构） `error_log`

### 类型与变量
`gettype` `get_class` `class_exists` `is_int`/`is_integer`/`is_long` `is_float`/`is_double` `is_string` `is_array` `is_bool` `is_null` `is_numeric` `is_callable` `intval` `floatval`/`doubleval` `strval` `boolval` `defined` `define`

### 数组
`count` `array_key_exists` `in_array` `array_push` `array_pop` `array_shift` `array_unshift` `array_keys` `array_values` `array_merge` `array_slice` `array_reverse` `array_sum` `sort` `rsort` `array_map` `array_filter` `array_reduce` `usort` `array_flip` `array_unique` `array_search` `array_key_first` `array_key_last` `array_column` `array_combine` `array_chunk` `array_fill` `range`

### 字符串
`strlen` `substr` `str_replace` `strpos` `stripos` `strrpos` `strripos` `trim` `ltrim` `rtrim` `strtolower` `strtoupper` `str_repeat` `str_pad` `implode`/`join` `explode` `str_split` `strrev` `strtr` `ucwords` `nl2br` `strip_tags` `htmlspecialchars` `str_contains` `str_starts_with` `str_ends_with` `sprintf` `number_format` `chr` `ord`

### 路径
`basename` `dirname`

### 数学
`abs` `min` `max` `intdiv` `pow` `round` `floor` `ceil` `sqrt` `mt_rand`/`rand` `gc_collect_cycles`

### 时间
`time` `date` `mktime` `checkdate` `microtime` `sleep` `usleep`（透明挂起）

### JSON
`json_encode` `json_decode` `json_last_error` `json_last_error_msg`

### 序列化
`serialize` `unserialize`（自有格式，与 PHP 文本不互通；跨 Worker 传值用）

### 回调
`call_user_func` `call_user_func_array`

### 包管理（开发期；CLI 命令的 PHP 可调用层，见 package-manager.md）
`pkg_new($dir, $name, $lib = false)` `pkg_add($proj, $source, $ref = "")` `pkg_remove($proj, $name)` `pkg_install($proj, $force = false)`

名字/条件非法抛 `ValueError`，IO 失败抛 `RuntimeException`；成功返回 `true`。

### 正则（vendor pcre2；见 PLAN/preg.md）
`preg_match` `preg_match_all` `preg_replace` `preg_split` `preg_grep` `preg_quote` `preg_last_error` `preg_last_error_msg`

- 模式语法对齐 PHP（分隔符/修饰符 `imsxADUuJn`）；编译失败/坏修饰符/坏分隔符 → 异常（PHP 的 Warning 按"诊断即异常"升级）
- `$matches` 需已初始化数组（无引用，见 PLAN/01 §10）；常量 `PREG_OFFSET_CAPTURE` / `PREG_UNMATCHED_AS_NULL` / `PREG_SET_ORDER` / `PREG_SPLIT_*` / `PREG_GREP_INVERT` / `PREG_*_ERROR` 已内置
- 后置：`preg_replace_callback`、数组形态 replace、PCRE2 JIT

### 其他
`exit`/`die` `spl_autoload_register`

## 透明异步 API（`Tphp\`）

| 函数 | 说明 |
|---|---|
| `Tphp\spawn($fn, ...$args)` | 启动协程，返回 `Tphp\Coroutine` |
| `Tphp\await($coro\|$chan)` | 等待完成/取值 |
| `Tphp\suspend($ms)` | 挂起当前协程 |
| `Tphp\current()` | 当前协程对象 |
| `Tphp\all($arr)` / `Tphp\any($arr)` | 聚合等待 |
| `Tphp\run($fn)` | 跑到全部完成返回 |
| `Tphp\async($callable)` | 包装成 `Tphp\Task`：调用即启动 |

### `async(EXPR)`：语法结构

`async(...)` 的**唯一实参是调用表达式**时，parser 把它改写成
`Tphp\spawn(fn() => EXPR)`——表达式整体在协程里执行，**不会先执行**；
自由变量按 spawn 那一刻按值捕获。

```php
async(p1(3, 4));                 // 等价 spawn(fn() => p1(3, 4))
$c = async(fetchUser($id));      // $id 在此刻取值
echo await($c);
$obj->spawn(task(1, 500));       // ->spawn(...) 同规则（spawn 为保留方法名）
```

单实参不是调用形态时（`async($callable)`）退化为包装，返回可复用的 `Task`：

```php
$p1 = async(p1(...));        // 包装一次
$p1(3, 4);                   // 每次调用 = 一次并发启动
$p1(3, 4)->wait();           // 启动并等待
```

**内置类**（均在 `Tphp\` 下）：
- `Tphp\Coroutine`（`isdone`/`getresult`/`cancel`/**`wait`**…）——`wait()` 是 `await()` 的对象式别名
- `Tphp\Channel`（`send`/`recv`/`close`/`len`，`new Tphp\Channel(cap)`）——支持多消费者/多生产者（等待者 FIFO 唤醒），可直接当信号量做并发限流
- `Tphp\TaskGroup`（`spawn`/`wait`/`count`）——**协程**任务组，`wait()` 按登记序返回结果数组
- `Tphp\Task`——`async()` 的返回物，`__invoke` 每次调用新建一个协程
- `Tphp\Thread`（`spawn`/`wait`/`detach`/`count`/`poll`）——**真 OS 线程**池（不是协程！），见下节

```php
use function Tphp\{async, await};
use Tphp\TaskGroup;

$group = new TaskGroup();
$group->spawn(task(1, 500));   // 自动包 thunk，task 不会先执行
$group->spawn(task(2, 900));
$group->wait();
```

## 文件 I/O

`file_get_contents` `file_put_contents` `fopen` `fread` `fwrite` `fgets` `fclose` `feof` `fseek` `file_exists` `is_file` `is_dir` `filesize` `unlink` `rename` `mkdir` `rmdir` `scandir`

`fopen` 返回 TStream 对象；网络读写透明挂起（libuv 线程池/事件循环）。

## 网络

`gethostbyname` `gethostbynamel` `fsockopen` `stream_socket_server` `stream_socket_accept` `http_get`（含 HTTPS，mbedtls）

## Thread（`Tphp\`，真 OS 线程池）

`Tphp\Thread` 是**真线程**（每个 `spawn` 起一个 OS 线程），不是协程。二者的关键差别：

| | `Tphp\TaskGroup`（协程） | `Tphp\Thread`（真线程） |
|---|---|---|
| 并行度 | 单线程内交错 | 多核真并行 |
| 阻塞调用 | 会卡住整个调度器（所有协程） | 只卡住该线程 |
| 捕获变量 | 同一份内存（引用语义） | 跨线程**深拷贝**（serialize 协议） |
| 适用 | I/O 密集、需大量并发 | 阻塞/计算密集，或在 UI 线程回调里跑长任务 |

```php
use Tphp\Thread;

$t = new Thread();
$t->spawn(fn() => heavy(1));        // 闭包入口：字节码跨线程共享
$t->spawn('lib_job');               // 字符串 = 全局函数名（main + require 进来的文件）
$t->spawn([Job::class, 'run']);     // [Cls, 'method'] 静态方法（可触发 autoload）
$r = $t->wait();                    // 按 spawn 序返回结果数组（阻塞）
```

- `spawn(Closure|string|array $entry)`——起一个一次性线程，入口三形态：
  - **Closure**——按 Program 索引序列化；捕获须可序列化（见下）
  - **string `'fn_name'`**——全局解析（main + `require` 的文件 + builtins），与 PHP 单请求全局符号表同心智；子线程继承同一张函数视图，入口跨文件调用照常
  - **`[Cls, 'method']`**——静态方法入口（类表每线程克隆，按类名+方法名重建）
- `wait()`——阻塞等待全部线程结束，按 spawn 序返回结果数组
- `detach()`——fire-and-forget：立即返回，线程自己跑完（父侧不跟踪、不 join）
- `count()`——当前跟踪中的线程数
- `poll()`——非阻塞收割已完成线程的返回值（不挂起协程，UI 线程可安全调用）

**捕获变量跨线程是深拷贝**：对象需实现 `__cloneState()`/`__restoreCloneState()` 才能被捕获（否则抛 `NotCloneableError`）；闭包本身可捕获（走 `T_CLOSURE` 序列化协议）。入口闭包捕获在构造时预检——捕获 FFI CData 指针、无 `__cloneState` 的对象（典型：方法内普通闭包自动捕获的 `$this`）直接报错并指引改用函数名/`[Cls,'method']`/`static function` 形态。

> **坑**：方法内定义的普通闭包会自动捕获 `$this`（PHP 对齐）。在实例方法里 `spawn` 请用 `static function () use (...)`（不绑 `$this`，`self::` 作用域仍生效），或改用函数名/静态方法形态——pebview `bindAsync` 即此改法。

**为什么 UI 回调里必须用真线程**：协程调度器在 C 栈阻塞（如 WebView 的 `webview_run`）时永远不会让出，
`spawn` 出来的协程永远得不到调度——表现为"点了按钮没反应"。真线程不依赖调度器让出，
在 FFI/C 回调里 `spawn` 立即开跑。

## Worker（`Tphp\`，真 OS 线程 + 消息传递）

```php
$w = new Tphp\Worker(function () {
    $me = Tphp\Worker::self();     // 子线程内的上下文（Tphp\Worker\Context）
    $me->post($msg);
    while (($m = $me->recv()) !== false) { ... }
});
$w->post($data);
$w->recv();
$w->on('message', $cb);   // stop drain 时分发
$w->stop();
$w->isRunning();
$w->id();
```

独立线程 + 独立 VM 堆；值跨线程 serialize 克隆；入口与 `Thread::spawn` 同三形态（Closure / string 全局函数名 / `[Cls,'method']`，见上节）。

## FFI

见 [ffi.md](ffi.md)：`FFI::cdef/new/free/memcpy/sizeof/...`、`FFI\CData` 下标与字段读写。

## lib\ PHP 层（phpc 打进 exe）

- `lib/std.php`：`str_title` `arr_second` `clamp_num`、常量 `TPHP_LIB`
- `require "std"` 按 bundle 名解析（无路径无扩展名）

## require / include

- 四形式：`require` `include` `require_once` `include_once`
- **字面量路径**：编译期内联；`*_once` 按路径全局去重；普通形式可重复展开（防环栈）
- **动态路径**（`require $p` / `require __DIR__."/x.php"`）：`OP_REQUIRE` 运行期求值 → bundle → pack zip → 磁盘；独立 parse+compile，函数/类注册进全局表
- 表达式形态：成功 `1`；`include` 失败 `false`；被包含文件顶层 `return $x` 的值作为 require 结果（动态路径）
- `require` 文件缺失 → 异常；`include` 文件缺失 → 静默跳过/false
- 循环 require：字面量加载期 `Circular require/include`；动态路径运行期 Call stack overflow（对齐原生普通 require 循环）
- 解析顺序：phpc bundle 表 → 磁盘 → pack zip（`source_root` 剥离 / 原样相对路径）

## 明确未实现（节选）

- `&$ref`、`extract`、`compact`、可变变量、`@`
- `preg_*`（M7+，需 vendor pcre2）
- `PDO`/SQLite、HTTP server、REPL（M7+）
- 被包含文件顶层 `return` 作为**字面量内联**形态的返回值（内联模型下 `return` 归属调用方；表达式固定 1/false；动态路径才支持文件 return 值）
- Worker 线程内动态 require 的函数对主线程可见性（inc_progs 为 per-VM，跨线程共享后置）
