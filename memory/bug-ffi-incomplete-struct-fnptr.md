# Bug: tphp FFI 不支持函数签名中的不完整结构体指针与函数指针 typedef

- **日期**：2026-10-30
- **发现场景**：用 `tphp.exe run` 跑 `C:\project\tphp\teb\tests\hello.php`（teb = 基于 civetweb 的 FFI 扩展包，`Tphp\Teb\Web` 通过 `FFI::cdef(include/teb.h, teb.dll)` 包 C 动态库）
- **状态**：**已修复（2026-10-30）**——tphp 侧「修复方向」两项全部落地 + 顺带三项；回归固化 `tests/ffi/test-ffi-incomplete-types.php`（232/232 全绿）；teb 端到端 `tests/hello.php` exit 0。teb 项目侧另发现 3 个独立 bug（见文末「修复记录」）。
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

---

## 修复记录（2026-10-30，全部落于 `ext/ffi/ffi.c`）

### tphp 侧（本 bug）
1. **`parse_struct_body` 查表 miss → 隐式不完整类型占位**（建 0-size/0-field struct/union 占位并登记 tag；enum 占位 size=4；后续完整定义经 `parse_named_body` **复用填充同一对象**，防双份登记）——覆盖参数位/typedef/成员位所有 `struct X` 引用
2. **顶层 `struct X *fn` 路径**：删错误回退（旧代码重读出 `*` 而非名字 → `unexpected token "*"`），保留 sname 走返回类型路径；查表 miss 同样建占位
3. **前向声明 `struct X;` 登记占位 tag**（旧实现忽略不登记 → 二次引用 unknown type）
4. **typedef 分支改走 `parse_declarator`**——接入已有的 `(*name)(params)` → `FFI_FUNC_PTR` 解析（函数指针 typedef 从此可用，回调 v1 trampoline 路径直接受益）
5. **按值不完整类型三消费点拦截**（`check_complete_byval`：解链数组后核心仍 0-size struct → ValueError，C 同非法）：`parse_function` 返回值/参数、结构体字段（普通+内嵌两路径）、`FFI::new`
6. **顺带：CData 字符串保活**（`FFCData.keep` 表 + `cdata_keep_str`）——字符串→`char*` 写入元素/字段存裸 `c` 指针，rvalue 非 interned 串释放即悬垂；`cdata_wrap` 补 `memset`（新字段防垃圾值）

### teb 项目侧（同场发现的独立 bug，已在用户 clone 上修复 1、3）
1. **`Web::start` options 数组缺 NULL 终止槽**：`new("char *[2]")` → civetweb 读 `options[2]` 越界（tphp 堆布局读到垃圾指针 → AV；真 PHP 恰好读到 0 而"活"——两端标准定义都是 UB）。已改 `char *[3]`（[2] memset 零即终止）
2. **`Web::request` 漏 `return $this;`**（声明 `: self` 返回 null → TypeError，真 PHP 同样会报）。已补
3. **`hello.php` 的 `// EXPECT: smoke=...` 与实现不符**：已修（2026-10-30，用户拍板「冒烟测试型 + 回调返回 body」契约）——`Web::run()` 改真阻塞（PHP 层 `sleep(1)` 循环保 uv loop 存活，供跨线程回调 drain，禁 FFI `Sleep()` 死等）；`request()` 包装闭包改为 `$cb` 返回响应体 → 拼 `HTTP/1.1 200` 头（Content-Length + Connection: close）→ `mg_write` → **`return 1`**（civetweb 签名 `int (conn,cbdata)`，返回 0 会被当未处理回 404）；`stop()` 置 `ctx=null` 防 `__destruct` 双重 `mg_stop`；`hello.php` 改为 start → 注册 handler → `http_get` 自请求（请求自带 `Connection: close`，服务端收 EOF 不挂）→ 打印 `smoke=<body>` → stop。实测：`smoke=hello from Tphp\Teb` exit 0；常驻形态外部 curl `/hi`→pong、`/nope`→404、进程存活

### 验证
- 8 形态实证表全 OK（对拍真 PHP 列全 ✅）；teb.h 全量 cdef OK；`mg_init_library/mg_version/mg_start/mg_stop` 双端一致；`mg_set_request_handler` 回调 trampoline 工作；hello.php exit 0
- `tests/ffi/test-ffi-incomplete-types.php` 新增（14 行 EXPECT，含 strcpy 保活探针），套件 **232 passed / 0 failed**

### 后续追加（同日晚，跨线程回调 + typedef alias 回归，套件 233/233）

1. **typedef alias 空签名 bug（本修复第 4 项的回马枪）**：顶层 `typedef (*cb)(...)` 走 alias 分支时只拷了 `fields/array_elem/ffi`，**漏拷 `fn_ret/fn_params/fn_n`** → 入表的是 `fn_ret=NULL, fn_n=0` 的空壳；闭包传入该 typedef 参数 → `closure_create` 解引用 `sig->fn_ret` → 0xC0000005。qsort 走内联声明（不经 alias）所以 parse-only 测试从未暴露。已补三字段拷贝 + `closure_create` 无签名 raise 兜底。
2. **teb handler 的真正崩溃点 = 跨线程回调**（`start+handler+http_get` 的 0xC0000005）：handler 在 civetweb worker 线程被调，该线程 `tphp_vm_current==NULL` 且 `g_cdata_cls` TLS 为 NULL，旧 trampoline **在 vm 判定之前 decode** → `cdata_wrap` 拿 NULL 类表即 AV。修复=方案 1 跨线程投递 marshaling（ffi.c CBQ/CBTask + `uv_async_send` 唤醒归属线程事件循环 drain 执行 + cond 回传结果，10s 超时零值返回；sched.c teardown 钩子 + 6 挂起点 `no current coroutine` 守卫；vm.c bounded 无 floor 逃逸），详见 MEMORY.md「2026-10-30 FFI 跨线程回调 marshaling」。
3. **端到端验证**：`tests/ffi/test-ffi-callback-cross-thread.php`（CreateThread 真外部线程 → 主线程执行 → `GetExitCodeThread`=42 验返回值回传）；teb 矩阵三连绿——`tmp-reach`：CALLBACK-REACHED + `http_get len=30` + `C: stopped` exit 0（原 AV）；`tmp-nohandler`：404 body 正常；hello.php 回归 exit 0。
