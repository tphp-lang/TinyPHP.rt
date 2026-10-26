# TinyPHP.rt

小型的 **PHP 8 运行时**——C11 实现，自有 opcode + VM 解释器，热函数走 IR baseline JIT。

```
源码 → lex/parse → 自有 opcode → VM 解释执行
                                    └─ 热函数 → IR → 机器码（baseline JIT）
```

- **无感异步**：fcontext 协程，业务函数不用标记；显式并发用 `async(EXPR)` / `spawn`
- **真线程 Worker**：独立 VM + 堆隔离 + Channel
- **IR baseline JIT**：热函数编译为机器码（dstogov/ir 后端，与 PHP 8.4+ 官方 JIT 同源）
- **FFI**：libffi 调 C 库（`FFI::cdef` / `CData`）
- **生成器**：`yield` / `send` / `getReturn`
- **正则**：`preg_*` 全批（vendor PCRE2）
- **单文件应用**：`tphp build app.php --with assets/ -o app`（Windows/Linux/macOS）
- **PHP 8 语义**：过程式 + 完整 OOP、数组全语义、异常、命名空间、PHP 8.4 属性钩子

当前版本 `0.0.2`。内置函数与异步 API 清单见 **[doc/stdlib.md](doc/stdlib.md)**。

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
use function Tphp\{async, await};
use Tphp\Thread;

// 无感异步：async(调用) 整体搬进协程，不阻塞主流程
async(sleep(1));
echo "main continues\n";

// 要结果就 await
$n = await(async(time()));

// 任务组：并发跑、统一收口
$t = new Thread();
$t->spawn(sleep(1));
$t->spawn(sleep(2));
$t->wait();

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

当前 **222 项** 全绿（含 bench / php-compat 种子）。详见 [doc/testing.md](doc/testing.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [doc/overview.md](doc/overview.md) | 项目概览与定位 |
| [doc/stdlib.md](doc/stdlib.md) | **内置功能与函数清单** |
| [doc/build.md](doc/build.md) | 构建与 CI（5 平台） |
| [doc/cli.md](doc/cli.md) | CLI：run / test / phpc / build |
| [doc/architecture.md](doc/architecture.md) | 运行时架构 |
| [doc/ffi.md](doc/ffi.md) | FFI API 与并发约定 |
| [doc/ffi-security.md](doc/ffi-security.md) | FFI 安全分析（PHP/txiki/Bun 对照） |
| [doc/testing.md](doc/testing.md) | 测试协议与目录 |
| [doc/package-manager.md](doc/package-manager.md) | 内置包管理：new/add/remove/install/upload |
| [PLAN/](PLAN/) | 完整设计规格（语法/功能/里程碑/扩展/包管理） |

## 项目结构

```
src/     运行时核心引擎（~2 万行 C）
ext/     内置扩展层（builtin/ ffi/ net/ io/ worker/ json/ serialize/ pkg/）
lib/     PHP 层 stdlib（phpc 打进 exe）
tests/   测试套件
deps/    vendor：libuv mimalloc mbedtls libffi miniz asm pcre2 ir(JIT)
PLAN/    设计文档（权威规格）
doc/     用户/开发者文档
tools/   辅助脚本（phpt 转换等）
```

## CI

GitHub Actions：**Windows x86_64 / Linux x86_64 / Linux aarch64 / macOS aarch64 / macOS x86_64（交叉）**——构建、全量测试、pack smoke，产物上传 artifact。

## 许可与参考

- 架构与部分代码模式参考 [php-src](https://github.com/php/php-src)、[txiki.js](https://github.com/saghul/txiki.js)
- fiber 汇编抄自 php-src `Zend/asm`（boost.context）
