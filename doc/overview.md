# 项目概览

TinyPHP.rt 是一个用 **C11** 实现的小型 **PHP 8 运行时**：源码 → 自有 opcode → 自研 VM 解释执行，热函数再经 **IR baseline JIT** 编译为机器码。

**不是** AOT；执行模型以解释器为本体，JIT 只覆盖热函数，语义兜底永远是解释器——「小型」指运行时内核（约 3.5 万行 C）。

## 定位

与姊妹项目 [TinyPHP]（编译 PHP 子集到 C，AOT）互补：

| | TinyPHP.rt | TinyPHP |
|---|---|---|
| 模式 | 解释器（opcode + VM）+ 热函数 JIT | AOT 编译到 C |
| 动态特性 | 全保留（`$fn()`、`new $cls` 等） | 受限子集 |
| 语义目标 | 接近完整 PHP 8 | 严格子集 |

## 硬性目标

1. **无感异步** —— 无色协程：业务函数不用标记，同步写法透明挂起；显式并发用 `async(EXPR)` / `spawn`
2. **真线程** —— Worker：独立 VM + 堆隔离 + Channel 消息传递
3. **单二进制发布** —— stdlib 用 PHP 写、打包期编进 exe（txiki.js 模式）
4. **PHP 8 语法** —— 过程式 + 完整 OOP，用户可见语义对齐 PHP 8.x
5. **性能** —— 持平或超越开 tracing JIT 的 PHP（IR baseline JIT，见 [../PLAN/ir-jit.md](../PLAN/ir-jit.md)）

## 技术栈

- **C11** + clang（Windows: msys2 clang64；Linux/macOS: 系统 clang）
- **vendor 依赖**：libuv（I/O）、mimalloc（分配器）、mbedtls + cacert（HTTPS）、fcontext 汇编、libffi、miniz、**PCRE2**（`preg_*`）、**dstogov/ir**（JIT 后端，与 PHP 8.4+ 官方 JIT 同源）
- **构建**：`build.cmd`（Windows）/ `build.sh`（Linux/macOS），无 Make/CMake
- **CI**：GitHub Actions 5 平台（win/linux×2/macos×2）+ 独立 perf 对比 workflow

## 文档导航

| 文档 | 内容 |
|---|---|
| [stdlib.md](stdlib.md) | **内置功能与函数清单** |
| [build.md](build.md) | 各平台构建与 CI |
| [cli.md](cli.md) | CLI 命令手册 |
| [architecture.md](architecture.md) | 运行时架构 |
| [ffi.md](ffi.md) | FFI 使用与约定 |
| [ffi-security.md](ffi-security.md) | FFI 安全分析（PHP/txiki/Bun 对照） |
| [package-manager.md](package-manager.md) | 内置包管理：new/add/remove/install/upload |
| [testing.md](testing.md) | 测试体系与跑法 |
| [../PLAN/](../PLAN/) | 完整设计规格（语法/功能/里程碑/扩展指南/包管理） |
