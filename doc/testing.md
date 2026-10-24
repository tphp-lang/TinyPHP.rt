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
| `tests/async/` | 协程：spawn/await/cancel/Channel/死锁/IO 并发 |
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

环境：Windows x86_64；PHP 8.5.1 NTS（opcache 开/JIT=disable 为公平参照）；tphp = L1–L3 后。JIT tracing 列仅参考（微基准下预热都不够，无感）。

| 基准 | tphp | PHP(无JIT) | 差距 |
|---|---|---|---|
| fib(28) | 109.3 | 29.3 | 3.7× |
| call 1e6（闭包） | 82.8 | 30.5 | 2.7× |
| array-push 1e6 | 296.8 | 20.1 | **14.8×** |
| array-rmw 1e5 | 15.3 | 1.9 | **8.1×** |
| array-read 1e6 | 66.1 | 17.5 | 3.8× |
| kv-read 1.6e6 | 78.0 | 22.6 | 3.5× |
| **string .= 4e4** | **292.4** | **0.9** | **325×** ⚠️ |
| throw/catch 1e5 | 109.9 | 30.4 | 3.6× |
| serialize 2e3 | 114.1 | 50.2 | 2.3× |
| json 2e3 | 301.0 | 198.0 | 1.5× |
| object 1e5 | 46.1 | 9.9 | 4.7× |

**发现**：① `string .=` 是 325× 单点灾难——tstr_concat 不可变全量重分配（O(n²)）vs zend_string rc=1 原地扩容；② array-push 15× = packed 写路径（L2 后置项，现有数据支撑）；③ 解释器正常差距带 2.7–4.7×；json/serialize 已 1.5–2.3×。**下一步**：`.=` in-place 优化 → packed 写路径。

## php-compat 对拍

```powershell
powershell -File tools/convert-phpt.ps1 -Src <php-src>/Zend/tests -Dst tests/php-compat -Limit 80
```

只收纯 `--EXPECT--` 小文件，转换后**只保留 tphp 全绿的**——失败是能力缺口，补能力后再转。
