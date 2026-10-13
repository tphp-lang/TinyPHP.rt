# 运行时架构

详细规格见 [../PLAN/03-architecture.md](../PLAN/03-architecture.md)。本文是简明导览。

## 编译管线

```
PHP 源码
  → lex.c        词法（HTML/PHP 切换、双引号插值分段）
  → parse.c      递归下降语法（优先级表驱动）
  → compile.c    AST → opcode（槽位/跳转/写回链）
  → Program { main TFunc + funcs[] + 常量池 }
  → tphp_prog_freeze  类表 flatten + INIT_FCALL ic 急解析（跨线程只读）
```

「编译」= **加载期**（lex→parse→AST→字节码），产物是自有 opcode，**永不生成机器码**。

## 执行管线

```
tphp_vm_run
  → tphp_sched_run          创建 main 协程 + scheduler 协程
  → vm_run_until_done       CALL 分发解释器（vm.c）
      OP_* handlers → tphp_op_handlers[]
      函数调用：INIT_FCALL → SEND → DO_FCALL → RECV → RETURN
      异常：per-function 异常表 + longjmp unwind
```

### 协程（无感异步）

- **fiber.c**：boost.context fcontext（抄 php-src `Zend/asm`），独立栈 + guard page，栈池复用
- **sched.c**：scheduler fiber 驱动就绪队列；挂起点在 `tphp_fiber_suspend`（builtin 深处冻结 C 栈）
- **TExecCtx**：每协程完整 VM 状态（帧链/调用栈/参数区/异常），切换 = 整块拷贝
- **ASAN 注解**：`__sanitizer_start/finish_switch_fiber`（php-src 协议）

### 透明 I/O

libuv 事件循环；文件 I/O 走线程池（挂起等待），网络真异步。HTTPS 用 mbedtls，根证书内嵌 `cacert.c`。

死锁判据：`uv_run` 返回 0 且仍有挂起协程 → `DeadlockError`。

### Worker 线程

- 每 Worker：独立 `TPhpRuntime`（VM 堆/intern/uv_loop/调度器全 TLS 隔离）
- mailbox = `uv_mutex + 消息链 + uv_async`；值跨线程走 serialize 克隆
- 字节码 freeze 后跨线程只读共享

## 值与内存

- **TVal** 16 字节：`{ union u; TType t }`
- 数组：插入序哈希（开址 + 墓碑），键边界规范化（`"8"`→int 8，`"08"` 留字符串）
- GC：标记-清扫（环回收）；acrc 释放走 `tval_release`；interned 串不参与 rc
- 分配：mimalloc 包装层（`tm_malloc`），与 ASAN 互斥

## 目录

```
src/     运行时核心（lex/parse/compile/vm/sched/fiber/io/net/ffi/...）
lib/     PHP 层 stdlib（phpc 打进 bundle）
tests/   测试（见 testing.md）
deps/    vendor 依赖
PLAN/    设计规格
doc/     用户/开发者文档（本目录）
```
