# 测试

## 快速跑

```powershell
# 全量（Windows / CI 各平台）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run.ps1

# 或内置 runner（单目录）
./build/tphp test tests/lib
./build/tphp.exe test tests/lang
```

- 退出码 0 = 全绿；非 0 = 有失败
- 非 Windows 自动 **SKIP** 6 个 FFI Windows-only 测试（msvcrt/user32 等）

## EXPECT 协议（txiki 风格）

一例一文件 `tests/**/test-*.php`，文件内注释断言：

```php
<?php
// EXPECT: hello
// EXPECT-EXIT: 0        // 可选，默认 0
echo "hello\n";
```

- **精确匹配** stdout 每一行
- `EXPECT:` 冒号后**只跳 1 个分隔空格**，缩进保留（`  [0]=>` 要对齐 var_dump 的缩进）
- 失败时 runner 打印逐行 diff；若异常退出，自动跑 `--dump-opcodes` 区分编译崩/执行崩

## 目录

| 目录 | 内容 |
|---|---|
| `tests/lang/` | 语言核心：算术/数组/字符串/控制流/闭包/命名空间 |
| `tests/oop/` | OOP：类/接口/trait/enum/魔术方法/可见性/属性钩子 |
| `tests/async/` | 协程：spawn/await/cancel/Channel/死锁/IO 并发；真线程 `Tphp\Thread`（spawn/wait/detach/poll） |
| `tests/worker/` | Worker：mailbox/克隆/stop/ping-pong |
| `tests/ffi/` | FFI：CData/struct/union/系统库/拒绝预处理 |
| `tests/lib/` | stdlib：json/date/array/require/bundle |
| `tests/pkg/` | 包管理：new/add/remove/install 骨架与依赖图 |
| `tests/bench/` | 性能基线：fib(28)、1e6 push、读改写（耗时走 stderr） |
| `tests/php-compat/` | php-src `.phpt` 对拍种子（见目录内 README） |
| `tests/standard/` | ext/standard 对拍（strpos 族 offset 语义 / nl2br / basename / dirname / is_file，基准 php-8.5.1） |

## 写新测试

1. `tests/<dir>/test-<name>.php`
2. 顶部或行间写 `// EXPECT:` 期望行
3. 跑 `run.ps1` 或 `tphp test tests/<dir>`
4. **纯 ASCII，UTF-8 无 BOM**（中文注释曾导致换行被吞的坑）

## 内存问题定位（ASAN）

mimalloc 只报"堆损坏"这类**症状**（如 `corrupted free list entry`），不给根因。定位内存 bug 时另建一份
ASAN 构建（**不定义** `TPHP_USE_MIMALLOC`，走 CRT malloc，ASAN 才能接管）：

```bash
# 编译 src/ + ext/ 带 -fsanitize=address，deps 对象复用主构建（不带 ASAN 也行）
clang -O1 -g -std=c11 -fno-omit-frame-pointer -fsanitize=address -D_GNU_SOURCE \
      -Isrc -Ideps/... -c src/*.c -o build/obj_asan/src_XXX.o
# 链接时把 deps 目标文件一起带上
clang -fsanitize=address build/obj_asan/src_*.o <deps .o> deps/libffi/lib/libffi.a \
      -o build/tphp_asan.exe <系统库>
```

跑法与正式版一致（`./build/tphp_asan.exe run tests/xxx.php`），ASAN 会直接给出
**分配点 / 释放点 / 使用点**三条栈，精确到行。`ASAN_OPTIONS=detect_leaks=0` 可关泄漏检查
（线程模型下 worker 线程退出时的挂起引用不算 bug）。

> 实例（2026-10-26）：`Tphp\Thread` 退出时报 `mimalloc: corrupted free list entry of size 64b`。
> ASAN 报的是 `heap-use-after-free`——`nm_thread_spawn` 把 Worker 对象同时交给
> 跟踪数组和 spawn 返回值两处持有，但 `tarr_append` 按约定**不做 addref**
> （见 `src/vm.c` 的 `op_assign`：`tval_addref(val)` 后才 `tarr_append`），
> 于是两处共用同一份引用计数：`wait()` 释放数组时提前 free，调用方帧随后再 release
> 一次 → 二次释放。修法是 `tarr_append` 前显式 `tval_addref`，让两处各持一份独立引用。

## 基准（bench，11 项）

```bash
./build/tphp.exe run tests/bench/test-bench-fib.php
# stdout: fib28=317811（恒定）
# stderr: BENCH fib(28) = 123.4 ms（回归基线）
```

结果断言恒定；耗时打 stderr 不进 EXPECT。分类覆盖（perf-roadmap §3 判据 3；方法论对齐 Bun：分类微基准，真实负载样本另行）：

| 文件 | 覆盖 |
|---|---|
| `test-bench-fib.php` | 递归调用密集 fib(28) |
| `test-bench-call.php` | 空函数+方法调用 1e6（调用开销基线） |
| `test-bench-array-push.php` | 数组追加 1e6 |
| `test-bench-array-read.php` | packed 顺序读 1e6 |
| `test-bench-array-rmw.php` | 读改写 1e5 |
| `test-bench-string.php` | 拼接+查找+substr（字符串密集） |
| `test-bench-exception.php` | throw/catch 1e5（异常路径） |
| `test-bench-coroutine.php` | spawn/await 1e4（协程切换） |
| `test-bench-serialize.php` | serialize/unserialize 2e3 |
| `test-bench-json.php` | json enc+dec 2e3 |
| `test-bench-object.php` | new+method+dtor 1e5（对象生命周期） |

2026-10-23 基线（Windows x86_64，仅供相对回归，跨机不可比）：call 298 / string 298 / json 294 / push 291 / fib 198 / throw 154 / serialize 113 / spawn 82 / arr-read 76 / new-dtor 65 / rmw 43（ms）。初步读数：调用 ~300ns/次（帧分配为头号嫌疑）、协程切换 ~8μs/次、packed 读 ~76ns/次。

2026-10-24 帧池化后（L1，perf-roadmap）：**call 175（-41%）/ fib 114（-42%）/ spawn 84（持平——协程瓶颈不在帧分配）**。池：VMFrame 头 freelist + slots 分档 4..256（>256 malloc），上限 256 防泄漏。

2026-10-24 packed array 读快路径后（L2）：**arr-read 76→64-77ms（-10~15%）**；rmw/push 持平（L2 只做读路径）。TArr.packed：纯追加连续 int 键且无 unset 时 data[i] 直取跳过哈希。

2026-10-24 键 intern（L3）：kv-read（字面量键 1.6e6 读）**64.4ms（vs 无 intern 66.4ms，-3%）**——tstr_eq hash 短路已覆盖大半，收益有限如实记录。新 bench `test-bench-kv-read.php` 入列。

## 对比 PHP 8.5.1（2026-10-24，同脚本 tools/bench-vs-php.php）

环境：Windows x86_64；PHP 8.5.1 NTS；**三方口径**：tphp / PHP（`opcache.enable_cli=1`，JIT 关）/ PHP+JIT（`opcache.jit=tracing`、`jit_buffer_size=128M`，已验证 status.jit.enabled）。tphp = L1–L3 + string .= in-place 后。

| 基准 | tphp | PHP | PHP+JIT | vs 无JIT | vs JIT |
|---|---|---|---|---|---|
| fib(28) | 114.7 | 26.5 | 10.9 | 4.3× | 10.5× |
| call 1e6（闭包） | 82.7 | 30.6 | 5.6 | 2.7× | 14.8× |
| array-push 1e6 | 295.4 | 21.5 | 11.5 | 13.7× | **25.7×** |
| array-rmw 1e5 | 10.3 | 2.1 | 1.8 | 4.9× | 5.7× |
| array-read 1e6 | 64.9 | 13.0 | 2.6 | 5.0× | **25.0×** |
| kv-read 1.6e6（变量键） | 160.2 | 34.4 | 7.2 | 4.7× | 22.3× |
| string .= 4e4 | 2.1 | 1.4 | 1.2 | 1.5× | 1.8× |
| throw/catch 1e5 | 112.8 | 31.8 | 30.2 | 3.5× | 3.7× |
| serialize 2e3 | 121.7 | 51.8 | 55.4 | 2.3× | 2.2× |
| json 2e3 | 310.3 | 198.7 | 199.7 | 1.6× | 1.6× |
| object 1e5 | 51.9 | 9.5 | 7.3 | 5.5× | 7.1× |

**结论**（详表与路线见 [../PLAN/perf-roadmap.md](../PLAN/perf-roadmap.md) §1.1）：JIT 不敏感簇（string/throw/serialize/json）1.6–3.7× 靠解释器关闭；JIT 敏感簇（fib/call/push/read/kv/rmw/object）vs JIT 5.7–25.7×，唯一通路 = 自研 IR JIT。

2026-10-25 L5 惰性桶同步后（bench-vs-php 同脚本）：**array-push 295.4→78.2ms（-74%）**，vs 无JIT PHP 3.6×（原 13.7×）、vs JIT 6.7×（原 25.7×）——packed append 只写 data[n]，桶在有人查时才从 data 全量重建（tarr_sync_buckets）；其余 bench 持平或小幅改善（json -6%、object -7%）。

2026-10-25 IR baseline JIT v1 上线（PLAN/ir-jit.md；TPHP_JIT=0 可关）：**fib(28) 13.2ms vs PHP tracing JIT 10.9ms = 1.2×（M1 达成）**，比 tphp 解释器（114.7ms）快 8.7×。仅 fib 等封闭 int 函数进 JIT 白名单，其余 bench 不变；全量 218/218。

## CI 性能对比（全平台，对 PHP 最强态）

`tools/perf-compare.ps1`：tphp 最强态（x86_64 开 IR JIT；aarch64 v1 为解释器）对 **PHP 最强态**（`opcache.enable_cli=1` + `opcache.jit=tracing` + 128M buffer，脚本内置 JIT 活性探针，未点火即报错）。bench 只含纯计算项（无异步/线程——PHP 原生没有）。**独立 workflow `.github/workflows/perf.yml`，仅 `workflow_dispatch` 手动触发**（不在 PR/push 的 ci.yml 里——共享 runner 上 bench 噪声大且费时，按需跑）。五个平台 job 一次全跑：

| 平台 | fib 门槛 | 说明 |
|---|---|---|
| Windows x86_64 / Linux x86_64 | 输入参数 `fib_gate`（默认 6.0×，硬门；0=仅报告） | tphp IR JIT 开启；本地实测 ~1.1–1.3× |
| Linux aarch64 / macOS aarch64 | 仅报告 | tphp JIT v1 未启用（x86_64 only），解释器对 PHP JIT 如实呈现 |
| macOS x86_64（cross） | 仅报告 | arm PHP 原生 vs x86_64 tphp（Rosetta），跨架构不设门 |

报告同时打进 step summary（`$GITHUB_STEP_SUMMARY`）。Zend optimizer 双态坑（§上文）已由 bench 变量键修复规避。

**历史修正**：①早期"JIT 微基准无感"是 CLI 未开 `opcache.enable_cli` 导致 JIT 从未点火——实测 fib/call/arr-read 提升 2.4–5.5×；②kv-read 常量键形态会被 Zend optimizer 间歇性整段折叠（进程间双态 ~22ms/~1.7ms，结果仍正确，`optimization_level=0` 后恒慢态；JIT 全程 false）——**跨运行时 bench 禁用"常量键+无副作用求和"形态**，已改变量键+校验。

2026-10-24 `.=` in-place 修复：**string 292.4→2.1ms**——TStr 加 cap 预留（非 interned：len<64→96 否则 +50%），op_concat 同槽快路 `tstr_append_inplace`（rc==1 且非 interned 原地追加；成功不走 wtake 防自释放）。

## php-compat 对拍

```powershell
powershell -File tools/convert-phpt.ps1 -Src <php-src>/Zend/tests -Dst tests/php-compat -Limit 80
```

只收纯 `--EXPECT--` 小文件，转换后**只保留 tphp 全绿的**——失败是能力缺口，补能力后再转。
