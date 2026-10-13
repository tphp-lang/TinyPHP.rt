# TinyPHP.rt

小型的 **PHP 8 解释型运行时**——C11 实现，自有 opcode + VM，不是 AOT/JIT。

```
源码 → lex/parse → 自有 opcode → VM 解释执行
```

- **无感异步**：fcontext 协程，同步写法透明挂起（无需 async/await）
- **真线程 Worker**：独立 VM + 堆隔离 + Channel
- **FFI**：libffi 调 C 库（`FFI::cdef` / `CData`）
- **单文件应用**：`tphp build app.php --with assets/ -o app`（Windows/Linux/macOS）
- **PHP 8 语义**：过程式 + 完整 OOP、数组全语义、异常、命名空间

当前版本 `0.0.1`，里程碑 **M0–M6 已完成**（语法/异步/Worker/FFI/stdlib/打包）。

## 构建

```bat
:: Windows（msys2 clang64）
build.cmd
```

```bash
# Linux
sudo apt-get install -y clang libffi-dev binutils
./build.sh

# macOS
brew install libffi
./build.sh
```

产物：`build/tphp(.exe)`。详细见 [doc/build.md](doc/build.md)。

## 快速试用

```bash
tphp -e 'echo "Hello, TinyPHP!\n";'
tphp run examples/01_hello.php
tphp -v
```

```php
<?php
// 无感异步
spawn(function () {
    sleep(1);
    echo "woken\n";
});

// FFI
$ffi = FFI::cdef("int abs(int j);", "msvcrt.dll");
echo $ffi->abs(-5), "\n";

// JSON
echo json_encode(["ok" => true]), "\n";
```

## 测试

```bash
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run.ps1
# 或
./build/tphp test tests/lib
```

当前 **107 项** 全绿（含 bench / php-compat 种子）。详见 [doc/testing.md](doc/testing.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [doc/overview.md](doc/overview.md) | 项目概览与定位 |
| [doc/build.md](doc/build.md) | 构建与 CI（5 平台） |
| [doc/cli.md](doc/cli.md) | CLI：run / test / phpc / build |
| [doc/architecture.md](doc/architecture.md) | 运行时架构 |
| [doc/ffi.md](doc/ffi.md) | FFI API 与并发约定 |
| [doc/testing.md](doc/testing.md) | 测试协议与目录 |
| [PLAN/](PLAN/) | 完整设计规格（语法/功能/里程碑/扩展） |

## 项目结构

```
src/     运行时核心（~3 万行 C）
lib/     PHP 层 stdlib（phpc 打进 exe）
tests/   测试套件
deps/    vendor：libuv mimalloc mbedtls libffi miniz asm
PLAN/    设计文档（权威规格）
doc/     用户/开发者文档
tools/   辅助脚本（phpt 转换等）
```

## CI

GitHub Actions：**Windows x86_64 / Linux x86_64 / Linux aarch64 / macOS aarch64 / macOS x86_64（交叉）**——构建、全量测试、pack smoke，产物上传 artifact。

## 许可与参考

- 架构与部分代码模式参考 [php-src](https://github.com/php/php-src)、[txiki.js](https://github.com/saghul/txiki.js)
- fiber 汇编抄自 php-src `Zend/asm`（boost.context）
