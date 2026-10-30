# Bug: tphp FFI 不支持函数签名中的不完整结构体指针与函数指针 typedef

- **日期**：2026-10-30
- **发现场景**：用 `tphp.exe run` 跑 `C:\project\tphp\teb\tests\hello.php`（teb = 基于 civetweb 的 FFI 扩展包，`Tphp\Teb\Web` 通过 `FFI::cdef(include/teb.h, teb.dll)` 包 C 动态库）
- **状态**：未修复（tphp 侧能力缺口）；已定位、已对照真 PHP 验证
- **严重度**：中（仅影响依赖「不完整结构体指针 / 函数指针 typedef」的 FFI 头；teb 这类 civetweb 封装直接中招）

---

## 症状

```
PHP Fatal error:  Uncaught ValueError: FFI::cdef(): unknown type "mg_connection"
in C:/project/tphp/teb/vendor/../src/Base.php:39
Stack trace:
#0 ~main(7)
#1 __construct(10)
#2 ffi(19)
#3 getFFI(39)
```

触发点：`teb/src/Base.php:39` → `self::$ffi = \FFI::cdef(file_get_contents($header), self::libPath());`
即 **tphp 在解析 `include/teb.h` 这段文本时就失败**，还没走到任何 C 函数调用。

---

## 根因

`tphp.exe run` 走的是 **tphp 自带的 FFI 实现（非 Zend 原生 FFI）**，其 `cdef` 解析器不支持两类标准 C 写法，而 civetweb 的 API 头（`teb.h`）大量依赖它们：

1. **函数签名 / 返回值里的「结构体指针」**（`struct mg_connection *`、`struct mg_context *`）
   —— `teb.h` 几乎每个函数都用这种参数。
2. **函数指针 typedef**（`typedef int (*cb)(...)`，如 `struct mg_callbacks` 的回调成员、`mg_set_request_handler` 的回调参数）。

> 注意：`teb.h` 顶部已有 `struct mg_context;` / `struct mg_connection;` 前向声明（不完整类型），在 C 与标准 PHP FFI 里完全合法——tphp 解析器只是没实现「不完整结构体」这条路径。

这不是 teb 的代码 bug：**同一份 `teb.h` 在真 PHP 8.4 的原生 FFI 下能正常解析**（见下「实证对照」）。

---

## 实证对照（关键证据）

在 tphp FFI 与真 PHP 8.4（`C:\env\php-8.4.13-nts-Win32-vs17-x64\php.exe`，FFI 开启）下分别跑同一组最小声明：

| 声明 | tphp FFI | 真 PHP 8.4 FFI |
|---|---|---|
| `void *make(void);` | ✅ OK | ✅ OK |
| 完整 `struct foo{int x;};` + `struct foo *make(void);` | ❌ `unexpected token "*"` | ✅ 解析通过 |
| `struct foo;` 单独前向声明 | ✅ 接受（但不登记 tag） | ✅ |
| `struct foo; struct foo *make(void);` | ❌ `unexpected token "*"` | ✅ |
| `typedef struct foo *foo_p; foo_p make(void);` | ❌ `unknown type "foo"` | ✅ |
| `struct foo { int x; struct foo *next; };` | ✅ OK（结构体**成员**内指针可用） | ✅ |
| `typedef int (*cb)(void*,void*); void set(cb f);` | ❌ `typedef needs a name` | ✅ |
| `long long add(long long,long long);` | ✅ OK | ✅ |

**结论**：tphp FFI 的缺口是
- 函数声明里的「`struct X *`」指针修饰符解析失败（结构体**成员**内的指针却支持）；
- 函数指针 typedef 完全不支持。

而真 PHP 只报「符号解析」类错误（因为测试里写的 `make`/`add` 是假函数、或 dll 未导出），**没有任何解析错误**——直接证明 `teb.h` 是合法 FFI 输入。

---

## 关联问题（独立，但会挡住「换真 PHP 跑 teb」这条路）

即便换真 PHP，`teb.h` 与 `teb.dll` 当前**不同步**：

- `teb.dll`（windows/x86_64）导出 78 个 `mg_*` 符号，`mg_init_library` / `mg_start` / `mg_set_request_handler` / `mg_stop` / `mg_write` / `mg_version` 等都在；
- 但 **`mg_websocket_write` 和 `mg_websocket_client_write`（`teb.h` 第 351–358 行）在 dll 里未导出**。
- 真 PHP 的 `FFI::cdef($cdef, $lib)` 会**立即**解析所有函数符号 → 抛 `Failed resolving C function 'mg_websocket_write'`。

验证：把 `teb.h` 第 351–358 行（两个 websocket write 声明）剔除后，真 PHP 成功 `FFI::cdef` 且 `mg_version()` 返回 `1.16`。

> 这是 **teb 项目自己的 header/DLL 脱节 bug**，与 tphp 无关；列在这里只是避免「切真 PHP 后又踩第二个坑」。

---

## 影响范围

- 任何 `cdef` 头里出现「函数参数/返回值为不完整结构体指针」或「函数指针 typedef」的 FFI 包，在 `tphp run` / tphp 打包产物下都会同样崩溃。
- tphp 自身测试套件里 FFI 用例若只用 `void*` / 标量 / 完整结构体，不受影响（与历史 MEMORY 记录的「M5.5 FFI」「FFI 回调 v1」不冲突——回调 v1 走的是 `FFI_FUNC_PTR` + libffi trampoline 的**独立**实现路径，不是 cdef 声明解析）。

---

## 绕行方案（临时）

1. **teb 改用真 PHP 运行**（最快）：用带 FFI 的 PHP CLI 跑，并先修 teb 自身脱节——把 `teb/include/teb.h` 第 351–358 行（`mg_websocket_write` / `mg_websocket_client_write`）注释/删除，或让 `teb.dll` 补导出这两个符号。teb 本质是普通 FFI 扩展包，不依赖 tphp 的 AOT/runtime。
2. **tphp 侧兜底**（未做）：在 `teb.h` 这类头里把所有 `struct X *` 参数/返回值替换成 `void *`，并把函数指针 typedef 改 `void *`。但这会破坏回调 API（`mg_set_request_handler` 仍需把 PHP callable 包成 C 函数指针），且等于放弃类型安全，**不推荐**。

---

## 修复方向（tphp 侧，治本）

在 tphp 的 FFI `cdef` 解析器（ext/ffi，对照 MEMORY 里 2026-10-22 的 FFI 回调实现落点）补齐：

1. 函数声明里 `struct X *` / `struct X **` 等指针修饰符的解析（不完整结构体按 `FFI\CData` 不透明句柄处理，与成员指针同构）。
2. 函数指针 typedef（`typedef <ret>(*name)(<params>);`）的解析与登记。

两项补齐后，`teb.h` 这类标准 FFI 头即可在 tphp 下直接加载。

---

## 复现命令（留档）

```bat
:: tphp 侧（必崩）
C:\project\tphp\TinyPHP.rt\build\tphp.exe run C:\project\tphp\teb\tests\hello.php

:: 真 PHP 侧（解析通过；teb.h 未修前会在 mg_websocket_write 符号解析处报 Failed resolving）
C:\env\php-8.4.13-nts-Win32-vs17-x64\php.exe C:\project\tphp\teb\tests\hello.php
```
