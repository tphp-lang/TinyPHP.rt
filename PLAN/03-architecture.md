# 03 · 实现架构

> 上级索引：[../PLAN.md](../PLAN.md)。解释型运行时：前端（lex/parse/compile）→ opcode → VM 解释执行 → 协程/调度器 → libuv。无 AOT、无 JIT。

## 1. 目录结构

```
TinyPHP.rt\
├── PLAN.md  PLAN\                  计划文档（本文件夹）
├── build.cmd / clean.cmd           纯 ASCII，clang 直调
├── build.sh  / clean.sh            Linux/macOS（uname 分支）
├── src\
│   ├── tphp.h                      总头文件：公共类型/注册表/错误码
│   ├── main.c                      CLI：-e / 脚本路径 / --dump-opcodes / -v
│   ├── mem.c          值+arena+GC          ├── value.c     TVal 16 字节值
│   ├── string.c       不可变+interned      ├── array.c     有序哈希表
│   ├── object.c       对象/属性槽          ├── class.c     继承扁平化/vtable/iface/trait/enum
│   ├── closure.c      闭包/捕获            ├── lex.c       手写词法器
│   ├── ast.h          tagged union         ├── parse.c     递归下降（优先级表驱动）
│   ├── compile.c      AST→opcode 两遍      ├── opcode.h    opcode 清单
│   ├── vm.c           CALL dispatch 引擎   ├── exception.c 抛出/展开/catch
│   ├── builtin.c      C 原生内置函数       ├── fiber.c     fcontext 协程
│   ├── sched.c        libuv 三 handle 调度 ├── async.c     Async\spawn/await/all
│   ├── worker.c       线程生命周期         ├── channel.c   Channel（同线程+跨线程）
│   ├── serialize.c    跨线程值克隆         ├── bundle.c    内置 PHP 库装载
│   ├── io_timer.c io_fs.c io_net.c io_proc.c   无感 I/O 内置实现
│   ├── error.c        错误对象/var_dump    └── bundle_data.c  打包器生成，勿手改
├── lib\                            PHP 写的标准库（打包进二进制）
│   ├── std.php json.php sprintf.php datetime.php async.php
├── tools\phpc.c                    lib\*.php → opcode blob → bundle_data.c
├── deps\
│   ├── libuv\                      vendor 源码（include/ + src/，Windows 侧约 40 文件）
│   ├── mimalloc\                   vendor 源码（malloc 层，M0；与 ASAN 构建互斥）
│   ├── mbedtls\                    vendor 源码（HTTPS/TLS，M4；配置裁剪到 TLS 客户端最小集）
│   ├── cacert.pem                  CA 根证书，内嵌进 exe（随依赖版本更新）
│   └── asm\                        抄 php-src Zend/asm 6 个 .S + LICENSE：
│       make/jump_x86_64_ms_pe_gas.S（Win）、sysv_elf_gas.S（Linux）、sysv_macho_gas.S（macOS）
├── tests\  lang/ oop/ async/ worker/ lib/ php-compat/(php-src 对拍) bench/ + run.cmd/run.sh（一例一文件 test-*.php）
└── examples\  hello.php oop.php async_demo.php worker_demo.php
```

## 2. 核心数据结构

```c
// 值：16 字节
typedef enum { T_NULL,T_FALSE,T_TRUE,T_INT,T_FLOAT,T_STR,T_ARR,
               T_OBJ,T_CLOSURE,T_ENUM_CASE,T_RES } TType;
typedef struct TVal { union { int64_t i; double d; void *p; } u; TType t; } TVal;
// 释放权责：TMP 帧销毁统一释放；CV 覆盖写时释放旧值；CONST 指只读常量池不参与 RC；堆结构带 uint32_t rc 头

typedef struct TStr { uint32_t rc,len,hash; bool interned,hash_done; char c[]; } TStr; // 不可变
typedef struct { TVal key; uint32_t val_off,hash_next; } ABucket;
typedef struct TArr { uint32_t rc,n,cap,hmask; int64_t next_idx;
                      ABucket *idx; TVal data[]; } TArr;   // data 保插入序 + 开放寻址索引，删除置 tombstone
typedef struct TClass { TStr *name; TClass *parent; uint8_t kind; /*class/iface/trait/enum*/
                        HashTable *methods; TProp *props; uint16_t n_props; TFunc **vtable; } TClass;
typedef struct TObj { uint32_t rc; TClass *cls; TObj *scope; TVal props[]; } TObj; // 属性槽位序 O(1)
typedef struct TFunc { TStr *name; uint32_t n_args,n_locals,n_tmps,n_ops;
                       TOp *ops; TVal *lits; TClass *cls; uint8_t flags; } TFunc;
typedef struct TClosure { uint32_t rc; TFunc *fn; TObj *self; TArr *captures; } TClosure;

// opcode：handler 指针内嵌（php-src CALL 模式起步），32 字节
typedef struct TOp { void *handler; uint16_t op; uint8_t t1,t2,tr; uint16_t ext;
                     uint32_t a1,a2,ar; } TOp;
// 操作数四分类：T_CONST(常量池下标)/T_CV(CV区下标)/T_TMP(TMP区下标)/T_UNUSED
// 帧：TFrame{ TFunc*; TOp *pc; TFrame *prev; TVal slots[]; } CV+TMP 连续排列，VM 栈页预分配，调用零堆分配
```

## 3. Opcode 清单（64 个）

| 组 | opcode |
|---|---|
| 基础 4 | NOP / CONST / COPY / FREE |
| 算术位 11 | ADD SUB MUL DIV MOD CONCAT BW_AND BW_OR BW_XOR SHL SHR |
| 比较 8 | SEQ(===) SNE(!==) EQ(==宽松,移植 compare_function) NE CMP(<=>) LT LE BOOL_NOT |
| 控制流 5 | JMP JMPZ JMPNZ JMP_LOOP SWITCH(ext 编 case 数) |
| 取值 6 | FETCH_DIM_R/W FETCH_PROP_R/W FETCH_STATIC FETCH_GLOBAL |
| 赋值 5 | ASSIGN ASSIGN_DIM ASSIGN_PROP ASSIGN_OP(ext 编 +=/./= 等) ASSIGN_GLOBAL(global 别名写) |
| 调用 8 | INIT_FCALL(调用点缓存) INIT_MCALL INIT_NEW SEND_VAL SEND_VAR RECV DO_FCALL RETURN |
| OOP 4 | NEW CLONE INSTANCEOF FETCH_CLASS（动态类名 new $cls 天然支持） |
| 构造 3 | CLOSURE ARRAY_INIT(ext: list/dict) ARRAY_APPEND |
| 异常 3 | THROW CATCH LEAVE |
| 其他 7 | TYPE_CHECK ISSET_DIM ISSET_PROP INC DEC ISSET_GLOBAL UNSET_GLOBAL |

POW/match/array_map 等全部下沉 PHP 库层；`echo` 编译为 `INIT_FCALL print`。`global $a` 别名变量：函数内读/写/isset/unset 分别编译为 FETCH_GLOBAL / ASSIGN_GLOBAL / ISSET_GLOBAL / UNSET_GLOBAL（编译期别名，无引用机制）。**UNSET_GLOBAL 语义 = 解除别名**（清槽位别名标记 + 置 UNDEF，**不动全局表**），对齐 PHP `global $x; unset($x);` 仅解绑、之后 `$x` 回归普通局部变量的行为——槽位的别名标记运行期存于帧上。`|>` 管道：编译期把左操作数注入右侧调用表达式的第一个 SEND；右侧为 first-class callable `f(...)` 形式时特判为单参调用 `f($x)`（对齐 PHP 8.5）。

## 4. 异步落地（VM 层标准路径）

```
内置 I/O 函数(io_*.c) 拆 begin/续段两段：
  begin: 发起 uv 操作 → 注册回调 + TCont{fiber} → fiber_suspend()
  uv 事件回调 → sched_wake(fiber)
  调度器 resume → 续段执行 → 返回值压栈 → VM 继续
```

调度器（txiki 三 handle 模式，全部 uv_unref）：
- `uv_prepare`：轮首，把被 uv 回调唤醒的 fiber 装入运行队列
- `uv_idle`：微任务/待 resume 协程存在时阻止 loop 空睡
- `uv_check`：轮尾 drain 就绪队列（轮转执行，每 fiber 跑到再次挂起）
- `uv_async`：stop 信号 + worker mailbox 唤醒
- 主体 `uv_run(UV_RUN_DEFAULT)`；全体挂起且无事件 → DeadlockError

Fiber：boost.context fcontext（抄 `Zend/asm`），栈 mmap/VirtualAlloc + guard page，默认 2MB 可调；**栈池复用**（回收的栈进池供新协程复用，防批量 spawn 内存爆炸，见 PLAN/04 风险 #9）。

io_timer：每个 timer 单独 `uv_timer_t` 堆分配 + `uv_close` 回调销毁（照抄 txiki `src/timers.c`，不建 handle 池）。HTTPS：`http_get`/`file_get_contents` 的 `https://` 走 mbedtls 握手（uv_tcp 之上接 TLS 层），根证书从内嵌 cacert.pem 加载。

## 5. Worker 落地（txiki 模式）

- `uv_thread_create` + `uv_sem` 握手（构造返回时子 runtime 已就绪）
- 子线程独立 TPhpRuntime（VM 栈/intern 动态区/uv_loop/调度器全独立）
- mailbox = `uv_mutex_t + 消息链表 + uv_async_send` 唤醒对端 loop；绝不跨线程共享 VM 内部数据
- 父线程独占生命周期：Stop → uv_async → uv_thread_join → Free
- 值克隆：自定义序列化（约 400 行，backref 去重；对象走 `__cloneState()` 协议）
- 只读共享：lib 字节码 + 启动冻结的 interned 字符串跨线程指针共享

## 6. 打包管线（单二进制）

```
lib\*.php --(tools/phpc.c，与 src 共享 lex/parse/compile 编译单元)-->
  每文件 opcode blob（TFunc 表+常量池+TOp 数组，小端定长）
  → src\bundle_data.c:
      const uint8_t tphp_bundle_std[] = {...};
      const TBundle tphp_bundles[] = { {"std",...}, {"json",...} };
运行期 require 'json' → 查表 → 反序列化为 TFunc/TVal
```

### 6.1 原生资产打包器（✅ M6.1 已实现，跨平台）

phpc 管 PHP 字节码（编译期 bundle_data.c）；原生文件打包与 phpc **正交**，FFI 层零改动：

- 构建：`tphp build app.php --with vendor\ --with ext\bar.dll -o app`
  `--with` 接受**文件或目录**（可重复）：目录递归收全部非 .php 文件（.php 默认排除并警告），
  **路径原样进包**（保留目录前缀：`--with vendor\ssl\foo.dll` → zip 内 `vendor/ssl/foo.dll`；
  绝对路径退回文件名/目录内部相对）
  miniz 打 zip + manifest.json（relpath/sha1/size/type；type 按后缀分类：`.dll/.so/.dylib` → `dll`，其余 `data`）
  嵌入方式：
  - **Windows**：PE RCDATA `TPHP_BUNDLE`（zip）+ `TPHP_MAIN`（脚本源码）
  - **Linux**：ELF section `.tphp_bundle`（objcopy 注入；失败回落 footer）
  - **macOS**：文件尾 footer 容器 `[zip][u64 le size][magic "TPHPBND1"]`
  - Unix 主脚本：zip 内 `__main.php`
- 运行期（main 最早期）：
  zip **常驻内存**（Bun 式）：数据资产首选经 `__DIR__` 相对读——磁盘未命中时
  io 层按打包时记录的源码根（`__source_root` zip 条目）剥离前缀、直读内存 zip，**不落盘**；
  `ASSET_ROOT`（`tphp://asset/`）是直接访问包根的次要前缀路径。manifest 中
  `type=dll`（`.dll/.so/.dylib`）按**等效相对路径**提取到 **exe 同级**
  （`<exedir>/<relpath>`；磁盘已存在且 sha1 与清单一致则跳过，
  否则校验 zip 侧 sha1 后原子写 .tmp → rename）；FFI 用包内相对路径命中清单 →
  `<exedir>/<relpath>` 绝对加载（`LOAD_WITH_ALTERED_SEARCH_PATH`，依赖走应用目录默认搜索）
  无嵌入 → 零开销正常启动；有嵌入主脚本且无 CLI 参数 → 执行
  防护：zip-slip（拒绝 `..`/绝对路径/盘符）；DLL 提取 sha1 校验
- CI 对每个平台跑 Pack smoke（build → `__DIR__` 相对读断言 → 断言数据不落盘）

## 7. 构建脚本要点

```bat
rem build.cmd（纯 ASCII）
set CFLAGS=-O2 -std=c11 -Wall -Wextra -g -fno-omit-frame-pointer -D_GNU_SOURCE
clang %CFLAGS% -Isrc -Ideps\libuv\include src\*.c deps\libuv\src\*.c deps\libuv\src\win\*.c ^
  deps\asm\x86_64_ms_pe*.S -o build\tphp.exe -ldbghelp -lws2_32 -ladvapi32 -luser32 -liphlpapi -lpsapi -lshell32
```

- M0 起把 `deps\mimalloc\src\*.c` 加入编译；M4 起加 `deps\mbedtls\library\*.c`（`MBEDTLS_CONFIG_FILE` 裁剪）与 cacert 内嵌
- **mimalloc 接入方式（txiki 同款）**：链接 `mimalloc-static`，由我们自己的 mem.c 分配器包装层调 `mi_malloc/mi_free`（对应 txiki 的 `TJS__HAS_MIMALLOC` 宏），**不做 CRT malloc interpose**——MinGW 下 interpose 与 CRT/libuv 内部分配纠缠有坑；只需保证 VM 堆统一走 mem.c
- M6 起两阶段：先编 tools\phpc.exe → 生成 src\bundle_data.c → 再编主程序
- build.sh：Linux 用 sysv_elf_gas.S + -lpthread -ldl -lm；macOS 用 sysv_macho_gas.S + CoreFoundation
- libuv 直接 vendor 源码入库（txiki 同款），build.cmd 显式列 `deps\libuv\src\win\*.c`
