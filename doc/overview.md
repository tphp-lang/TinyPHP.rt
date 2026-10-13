# 项目概览

TinyPHP.rt 是一个用 **C11** 实现的小型 **PHP 8 解释型运行时**：源码 → 自有 opcode → 自研 VM 解释执行。

**不是** AOT、**不是** JIT、不生成机器码——「小型」指运行时内核（约 3 万行 C）。

## 定位

与姊妹项目 [TinyPHP]（编译 PHP 子集到 C，AOT）互补：

| | TinyPHP.rt | TinyPHP |
|---|---|---|
| 模式 | 解释器（opcode + VM） | AOT 编译到 C |
| 动态特性 | 全保留（`$fn()`、`new $cls` 等） | 受限子集 |
| 语义目标 | 接近完整 PHP 8 | 严格子集 |

## 硬性目标

1. **无感异步** —— 无色协程：同步写法透明挂起（类 JS 并发体验，无需 async/await）
2. **真线程** —— Worker：独立 VM + 堆隔离 + Channel 消息传递
3. **单二进制发布** —— stdlib 用 PHP 写、打包期编进 exe（txiki.js 模式）
4. **PHP 8 语法** —— 过程式 + 完整 OOP，用户可见语义对齐 PHP 8.x

## 已完成里程碑

| 里程碑 | 内容 |
|---|---|
| M0 骨架 | 构建脚本、vendor 依赖、CLI |
| M1 值+表达式 | 词法/语法/数组全语义、GC、VM |
| M2 函数+异常 | 闭包、命名参数、管道 `\|>`、异常体系 |
| M3 OOP | class/interface/trait/enum、命名空间、魔术方法 |
| M4 协程+I/O | fcontext 纤维、libuv 事件循环、透明 HTTP/文件 I/O、HTTPS |
| M5 Worker | 线程隔离、serialize 克隆、mailbox |
| M5.5 FFI | libffi cdef/CData、系统库绑定 |
| M6 打包+stdlib | JSON/date 等 builtin、require 内联、phpc、`tphp test`、跨平台 `tphp build` |

## 技术栈

- **C11** + clang（Windows: msys2 clang64；Linux/macOS: 系统 clang）
- **vendor 依赖**：libuv（I/O）、mimalloc（分配器）、mbedtls + cacert（HTTPS）、fcontext 汇编、libffi、miniz
- **构建**：`build.cmd`（Windows）/ `build.sh`（Linux/macOS），无 Make/CMake
- **CI**：GitHub Actions 5 平台（win/linux×2/macos×2）

## 文档导航

| 文档 | 内容 |
|---|---|
| [build.md](build.md) | 各平台构建与 CI |
| [cli.md](cli.md) | CLI 命令手册 |
| [architecture.md](architecture.md) | 运行时架构 |
| [ffi.md](ffi.md) | FFI 使用与约定 |
| [testing.md](testing.md) | 测试体系与跑法 |
| [../PLAN/](../PLAN/) | 完整设计规格（语法/功能/里程碑/扩展指南） |
