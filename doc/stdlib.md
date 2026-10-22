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
- 命名空间：`namespace`、`use` / `use as` / `use function` / `use const`
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
| `PHP_VERSION` | 运行时版本号（如 `"0.0.1"`，来自 `TPHP_VERSION`） |
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

### 其他
`exit`/`die` `spl_autoload_register`

## 透明异步 API（`Async\`）

| 函数 | 说明 |
|---|---|
| `Async\spawn($fn, ...$args)` | 启动协程，返回 `Async\Coroutine` |
| `Async\await($coro\|$chan)` | 等待完成/取值 |
| `Async\suspend($ms)` | 挂起当前协程 |
| `Async\current()` | 当前协程对象 |
| `Async\all($arr)` / `Async\any($arr)` | 聚合等待 |
| `Async\run($fn)` | 跑到全部完成返回 |

**内置类**：`Async\Coroutine`（`isdone`/`getresult`/`cancel`…）、`Async\Channel`（`send`/`recv`/`close`/`len`，`new Channel(cap)`）。

## 文件 I/O

`file_get_contents` `file_put_contents` `fopen` `fread` `fwrite` `fgets` `fclose` `feof` `fseek` `file_exists` `filesize` `unlink` `rename` `mkdir` `rmdir` `scandir`

`fopen` 返回 TStream 对象；网络读写透明挂起（libuv 线程池/事件循环）。

## 网络

`gethostbyname` `gethostbynamel` `fsockopen` `stream_socket_server` `stream_socket_accept` `http_get`（含 HTTPS，mbedtls）

## Worker（`tphp\`）

```php
$w = new tphp\Worker(function () {
    $me = tphp\Worker\Worker::self();
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

独立线程 + 独立 VM 堆；值跨线程 serialize 克隆；入口仅支持 Closure。

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
