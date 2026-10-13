# 内置功能与函数清单

当前已实现的内置能力。语法范围见 [../PLAN/01-syntax-spec.md](../PLAN/01-syntax-spec.md)。

## 语言特性

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

## 内置函数（C 原生，builtin.c）

### 输出与调试
`print` `var_dump` `print_r` `echo`（语言结构） `error_log`

### 类型与变量
`gettype` `get_class` `class_exists` `is_int`/`is_integer`/`is_long` `is_float`/`is_double` `is_string` `is_array` `is_bool` `is_null` `is_numeric` `is_callable` `intval` `floatval`/`doubleval` `strval` `boolval` `defined` `define`

### 数组
`count` `array_key_exists` `in_array` `array_push` `array_pop` `array_shift` `array_unshift` `array_keys` `array_values` `array_merge` `array_slice` `array_reverse` `array_sum` `sort` `rsort` `array_map` `array_filter` `array_reduce` `usort` `array_flip` `array_unique` `array_search` `array_key_first` `array_key_last` `array_column` `array_combine` `array_chunk` `array_fill` `range`

### 字符串
`strlen` `substr` `str_replace` `strpos` `stripos` `strrpos` `trim` `ltrim` `rtrim` `strtolower` `strtoupper` `str_repeat` `str_pad` `implode`/`join` `explode` `str_split` `strrev` `strtr` `ucwords` `nl2br` `strip_tags` `htmlspecialchars` `str_contains` `str_starts_with` `str_ends_with` `sprintf` `number_format` `chr` `ord`

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

## 明确未实现（节选）

- `&$ref`、`extract`、`compact`、可变变量、`@`
- `preg_*`（M7+，需 vendor pcre2）
- `PDO`/SQLite、HTTP server、REPL（M7+）
- `require_once` 的「一次」语义（当前按路径去重，所有 require/include 均去重）
