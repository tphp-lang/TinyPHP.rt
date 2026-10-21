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

## 写新测试

1. `tests/<dir>/test-<name>.php`
2. 顶部或行间写 `// EXPECT:` 期望行
3. 跑 `run.ps1` 或 `tphp test tests/<dir>`
4. **纯 ASCII，UTF-8 无 BOM**（中文注释曾导致换行被吞的坑）

## 基准（bench）

```bash
./build/tphp run tests/bench/test-bench-fib.php
# stdout: fib28=317811（恒定）
# stderr: BENCH fib(28) = 123.4 ms（回归基线）
```

结果断言恒定；耗时打 stderr 不进 EXPECT。

## php-compat 对拍

```powershell
powershell -File tools/convert-phpt.ps1 -Src <php-src>/Zend/tests -Dst tests/php-compat -Limit 80
```

只收纯 `--EXPECT--` 小文件，转换后**只保留 tphp 全绿的**——失败是能力缺口，补能力后再转。
