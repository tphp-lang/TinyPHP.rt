# TinyPHP.rt 实施计划（索引）

> 状态：已确认设计，按里程碑串行执行。计划确认时间：2026-10-09。
> 详细规格拆分在 `PLAN\` 文件夹中，本文档为索引与定位声明。

## ⚠️ 定位声明：这是解释型运行时，不是 AOT

**TinyPHP.rt = 解释器**：源码 → 编译为自有 opcode → **自有 VM 解释执行**。
- **不做 AOT、不做 JIT、不生成机器码/C 源码**——"小型"指运行时内核小（约 1.3 万行 C），不是编译器
- 因为是解释器，**动态特性全部保留**：`$fn()`、`new $cls`、`$obj->$method()`、`$cls::$method()`、`$obj->{'prop'}` 等运行期动态分发是基本能力（opcode 层天然支持，无加载期约束）
- 类型声明（int/string/union/readonly）只是**运行期校验 + 加载期辅助报错**，与 AOT 的"强类型子集"完全是两回事；strict 声明全局隐含开启，但语义校验发生在运行时
- 与 TinyPHP（姊妹项目）的关系：TinyPHP 编译 PHP 严格子集到 C（AOT），TinyPHP.rt 解释执行接近完整 PHP 8 语义。两者代码零共享、定位互补
- 抛掉 `&$ref` 引用的唯一原因：**引用模型与协程挂起、跨线程克隆、简单 GC 三者冲突**，与 AOT 无关

**术语澄清**：计划中的"编译"一律指 **源码 → opcode 的加载阶段**（lex → parse → AST → 字节码，与 php-src 内部 `zend_compile.c` 的"编译器"同义）——任何脚本执行前都要先过这一步，产物是自有 opcode 交给 VM 解释，**永不生成机器码**。因此"编译期报错"更准确的叫法是 **加载期报错**：脚本还没执行第一行就被拒绝的静态错误（语法错误、不支持的特性等），对应 PHP 7+ 的 `ParseError/CompileError`；而 `TypeError/DivisionByZeroError` 等是 opcode 执行时抛出的**运行期报错**。下文文档统一使用"加载期"。

## 硬性目标

1. **无感异步**：无色模型——同步写法透明挂起/恢复（像 JS 的并发体验，无需 async/await 着色）
2. **真线程**：Worker 模型——独立 VM 实例 + 堆隔离 + Channel 消息传递（像 C 的多核能力，无共享内存陷阱）
3. **单二进制发布**：stdlib 用 PHP 自写、发布构建期由 phpc 工具打包为自有 opcode 的 C 数组编入 exe（txiki.js 模式）
4. **PHP 8 语法完整版**：过程式 + 完整 OOP，用户可见语义对齐 PHP 8.x

## 已确认决策

| 决策点 | 结论 |
|---|---|
| 实现语言 | C11（可直接搬 zend_fibers.c / Zend/asm 现成实现） |
| 项目性质 | **解释型运行时**（opcode + VM），非 AOT、非 JIT |
| 语法范围 | 过程式 + 完整 OOP（M1 先过程式，M3 补 OOP） |
| I/O 层 | 直接 vendor libuv 源码（不用 submodule） |
| 数组数字字符串键 | **对齐 PHP**："8"→int 8；"08"/"8.5"/超 int64 保留字符串（M1 定死冻结） |
| == 宽松比较 | **完全对齐 PHP** 全部隐式转换规则 |
| 抛掉 &$ref | 因协程/克隆/GC 而非 AOT（见上） |
| HTTPS/TLS | vendor mbedtls + `cacert.pem` 根证书内嵌 exe（M4；libuv 无 TLS，txiki cacert.c 同款） |
| 分配器 | mimalloc vendor 垫在 malloc 层（默认开；与 ASAN 构建互斥，txiki 先例） |
| 语义对拍 | 从 php-src `Zend/tests` 抽 .phpt 子集做对拍测试层（见 PLAN/04 §2） |
| FFI | libffi vendor 进 deps（静态 .a + 头文件）；M5.5 已完成（含 dlopen/wchar/union/enum 常量） |

## 计划文档导航

| 文件 | 内容 |
|---|---|
| [PLAN/01-syntax-spec.md](PLAN/01-syntax-spec.md) | 语法兼容到什么程度：逐层 ✅/❌ 清单、不支持清单、行为差异、**验收代码示例**（M3 结束全跑通） |
| [PLAN/02-runtime-features.md](PLAN/02-runtime-features.md) | 运行时功能有哪些：透明异步 I/O 覆盖表、Async\ 完整 API、Worker 线程 API、stdlib 三层清单、CLI、**功能验收代码示例** |
| [PLAN/03-architecture.md](PLAN/03-architecture.md) | 实现架构：目录结构、核心数据结构、opcode 64 个、异步/线程/打包的 VM 层落地、构建脚本 |
| [PLAN/04-milestones-testing.md](PLAN/04-milestones-testing.md) | 里程碑 M0~M6（每阶段绑定语法+功能范围与验收标准）、测试策略、风险与规避 |
| [PLAN/05-extension-guide.md](PLAN/05-extension-guide.md) | **贡献者规范**：内置扩展（builtin/内部类）开发模式、核心约定红线、错误处理与安全标准、工作流与上线检查清单 |
| [PLAN/pkg-manager.md](PLAN/pkg-manager.md) | **内置包管理器（无外部工具/无 git）**：mod.json/mod.lock、vendor 布局、PSR-4 autoload、new/add/remove/install/upload 命令规格、实施阶段（已全部实现） |
| [PLAN/perf-roadmap.md](PLAN/perf-roadmap.md) | **性能路线**：参照系三层（Zend/Bun/IR JIT）、优化时机四判据、解释器优化清单、IR 框架 JIT 路线（PHP 8.4 官方底座）、Bun 借鉴清单、防翻案结论 |
| [PLAN/generators.md](PLAN/generators.md) | **生成器（yield）**：帧保存模型（VMFrame 直存 Generator）、opcode 集、v1 范围（G1 基础/G2 send/G3 边界）、v2 后置（throw/from/dtor-finally）、已知差异 |
| [PLAN/preg.md](PLAN/preg.md) | **正则（preg_*）**：vendor pcre2（php-src 8.5.7 pcre2lib）、模式语法/修饰符映射、v1 函数集语义、错误模型决策（编译=异常/运行=返回值协议）、P1–P4 分批 |

## 技术栈与环境

- C11 + clang（MinGW64/ucrt64 或 clang64），目标 Windows/Linux/macOS
- 构建纯命令行脚本（不用 Make/CMake），`.cmd`/`.sh` 纯 ASCII
- CI：GitHub Actions `.github/workflows/ci.yml`——Windows x86_64（msys2 clang64）、Linux x86_64/aarch64、macOS x86_64/arm64，build + tests/run.ps1 + smoke
- vendored：libuv（I/O）、mimalloc（malloc 层）、mbedtls + cacert（HTTPS，M4）、fcontext 汇编（抄 php-src `Zend/asm/` 6 个 .S）、libffi（M5.5 FFI，静态库+头文件）
- 仓库：`C:\project\tphp\TinyPHP.rt`，CLI 名 `tphp`

## 参考项目

- [php-src](https://github.com/php/php-src) 本地：`C:\Users\28249\Desktop\php-src-php-8.5.11`
- [txiki.js](https://github.com/saghul/txiki.js) 本地：`C:\Users\28249\Desktop\txiki.js`

## 执行顺序

M0 → M1 → M2 → M3 → M4 → M5 → M6 串行推进；每阶段 `tests\run.cmd` 全绿 + 验收 CLI 实际输出贴给用户确认后才进下一阶段。
