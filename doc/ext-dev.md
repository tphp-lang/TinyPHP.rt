# C 扩展开发指南：把 PHP 功能封装进 tphp

> 面向：想把某个 **PHP 能力**（函数 / 常量 / 类与方法）用 C 实现、链入 tphp 二进制的开发者。
> 本文 = **步骤 + 可用 C 函数手册 + 完整配方**。权威规格：[../PLAN/ext-packages.md](../PLAN/ext-packages.md)（§2 manifest 契约 / §2.4 注册面 / §5 编译链入 / §6 lock）。
> 分野：源码树 `ext/` 的**内置**扩展（贡献者工作）见 [../PLAN/05-extension-guide.md](../PLAN/05-extension-guide.md)；PHP 依赖包见 [package-manager.md](package-manager.md)；脚手架设计见 [../PLAN/scaffold.md](../PLAN/scaffold.md)。ext 包铁律：**不进 `vendor/`、不进 autoload、无 `scripts`**。

## 0. 三十秒跑通

```bash
tphp new acme/hello --ext          # 1. 脚手架 → ./hello/（目录取最后一段）
$EDITOR hello/src/hello.c          # 2. 写封装（生成的桩 = 可编译的注册样例）
tphp ext --add ./hello             # 3. 编译链入（恒重链；下次运行即新 tphp）
tphp run hello/examples/usage.php  # 4. PHP 侧验证
```

## 1. 封装六步（路线图）

| # | 步骤 | 关键点 |
|---|---|---|
| 1 | **定 PHP 签名** | 先写清楚要暴露的样子：`function acme\hello\hello(string $who): string`、常量 `acme\hello\VERSION`、类 `acme\hello\Counter { __construct(int), add(int=1):int, get():int }`——C 只是实现，签名先定 |
| 2 | **`tphp new acme/hello --ext`** | 生成 mod.json / `src/hello.c`（**自注册最小三件桩**：常量+函数+空类，本身就是样例）/ `include/` / README / `.gitignore` / `examples/usage.php` |
| 3 | **写 C 实现** | 函数体四段：**读参数 → 判类型 → 干活 → 构造返回**；出错走 `tphp_vm_raise`。用什么 C 函数见 §2 手册，骨架照抄 §3 配方 |
| 4 | **在 init 里注册** | 三面：`tphp_ext_reg_const_str/int`、`tphp_ext_reg_fn`、类按 exclass 蓝本构造 `TClass` + `tphp_class_register_compile`。init 先于首脚本编译执行一次 |
| 5 | **核对 manifest** | `mod.json` `ext{}`：`sources`/`include`/`os`（分平台 libs/cflags/ldflags）/`tphp_abi`/`init` 符号名——字段表与禁项见 §4 |
| 6 | **链入 + 验证** | `ext --add`（恒编译重链，换像后**下次运行**生效）→ 跑 PHP 脚本断言输出。改了 C 再 `add` 即出新 tphp |

## 2. C 函数手册（封装用到的全部积木）

头文件用脚手架已 include 的四件：`pkg.h`（注册）、`value.h`（值）、`class.h`（类/类型名）、`opcode.h`（VM/异常）；参数校验另加 `builtin.h`。

### 2.1 读参数与判类型

C 函数原型（普通函数）：

```c
static TVal my_fn(VM *vm, TVal *args, int n);
```

| 要做的事 | 用什么 |
|---|---|
| 有几个实参 | `n`（+1 判可选参缺省：`n >= 1`） |
| 判类型 | `args[0].t`，标签枚举见下表 |
| 取整数 | `args[0].u.i`（`T_INT` 时） |
| 取浮点 | `args[0].u.d`（`T_FLOAT` 时） |
| 取字符串 | `tstr_val((TStr *)args[0].u.p)` + `tstr_len(...)`（`T_STR` 时） |
| 取数组 | `(TArr *)args[0].u.p`（`T_ARR` 时） |
| 取对象 | `(TObj *)args[0].u.p`（`T_OBJ` 时） |
| **PHP 同款类型检查** | `need_str(vm, &args[0], "acme\\hello\\my_fn", "who")`、`need_int(...)`（`builtin.h`——strict，不匹配直接抛 TypeError，文案对齐 PHP "must be of type X, Y given"；命中返回值本体） |

**类型标签**（`value.h` `TType`）：

| 标签 | 对应 PHP | 载荷 |
|---|---|---|
| `T_NULL` | `null` | — |
| `T_FALSE` / `T_TRUE` | `false` / `true` | —（**独立标签**，不是 int） |
| `T_INT` | `int` | `u.i`（int64） |
| `T_FLOAT` | `float` | `u.d`（double） |
| `T_STR` | `string` | `u.p` = `TStr *` |
| `T_ARR` | `array` | `u.p` = `TArr *` |
| `T_OBJ` | `object` | `u.p` = `TObj *` |
| `T_CLOSURE` | `Closure` | `u.p` |
| `T_ENUM_CASE` / `T_RES` | enum case / resource | `u.p` |

> 未限定语义的宽松处理用强转表（§2.5）；`need_*` 是 strict 语义（PHP 参数类型声明行为）。

### 2.2 构造返回值

| 值 | 写法 |
|---|---|
| 整数 | `TV_INT(42)` / `TV_INT(args[0].u.i + 1)` |
| 浮点 | `TV_FLOAT(3.14)` |
| 字符串 | `TV_STR(tstr_new(buf, strlen(buf)))`（先 `snprintf` 进栈 buf） |
| 空串 | `TV_STR(tstr_new("", 0))` |
| null | `TV_NULL` |
| 布尔 | `TV_TRUE` / `TV_FALSE` |
| 数组 | `TV_ARR(a)`（`a` = `tarr_new()` 的结果，见 §2.4） |
| 整数转串 | `TV_STR(tphp_int_to_str(v))`；浮点转串 `tphp_float_to_str(v)` |
| 属性里拿的（借用） | **先 `tval_addref(v)` 再返回**（否则对象属性被释放后你返回悬垂值） |

**所有权一句话**：返回给 VM 的值 = **转移所有权**（新构造的 `tstr_new/tarr_new` 天然 +1，直接返回）；**`args[]` 是借用**，不要 release，若要把某个实参存起来超过本次调用就先 `tval_addref`。

### 2.3 引用计数（堆值）

| 操作 | 写法 |
|---|---|
| 加引用 | `tval_addref(v)`（按值） |
| 减引用 | `tval_release(&v)`（按地址；归零自动析构） |
| 标量自动跳过 | `T_NULL/T_FALSE/T_TRUE/T_INT/T_FLOAT` 不参与计数，release 是 no-op——**可以无脑配对** |
| 深拷贝 | `tval_clone_deep(v)`（数组/COW 场景） |

配对纪律：**谁 +1 谁 −1**。从属性/`tarr_get` 拿出的引用要自己 `tval_addref` 或用完 `tval_release`；构造返回的新值不用再 release。

### 2.4 字符串与数组

**字符串**（`TStr *`）：

| 函数 | 说明 |
|---|---|
| `tstr_new(s, len)` | 新堆串（返回值直接 `TV_STR(...)`） |
| `tstr_intern(s, len)` | 程序级驻留串（类名/常量名等静态名用） |
| `tstr_concat(a, b)` | 拼接 → 新堆串 |
| `tstr_append_inplace(dst, s, n)` | 原地追加（`dst` 须是堆串） |
| `tstr_val(s)` / `tstr_len(s)` | 取 C 指针 / 长度 |
| `tstr_eq(a, b)` | 相等（指针或内容） |
| `tphp_int_to_str(i)` / `tphp_float_to_str(d)` | 数值转串 → 新引用 |

**数组**（`TArr *`，样例见 §3-R2）：

| 函数 | 说明 |
|---|---|
| `tarr_new()` | 新空数组 → `TV_ARR(a)` 返回 |
| `tarr_append(a, v)` | `$a[] = v`——**搬入**：把手里新鲜构造的 `v` 直接交给数组，之后**不要**再 release 它（json.c 同款写法） |
| `tarr_set(a, key, v)` | `$a[key] = v`（键先过 `tval_norm_key(&key)` 规范化） |
| `tarr_get(a, key)` | `TVal *`（NULL=不存在；取出值按 §2.3 处理引用） |
| `tarr_has(a, key)` / `tarr_count(a)` / `tarr_unset(a, key)` | `isset` / `count` / `unset` |
| `tarr_free(a)` | 错误路径提前销毁（成功返回给 VM 后由 VM 管） |

数组**遍历**暂无公开迭代器——参考 `ext/standard/`、`ext/json/json.c` 的既有写法。

### 2.5 强转与比较（PHP 语义）

| 函数 | 说明 |
|---|---|
| `tval_to_bool(v)` | `(bool)` 语义 |
| `tval_to_int(v)` | `(int)`——可能抛（NAN/OOB 按 PHP 8.5） |
| `tval_to_float(v)` | `(float)` |
| `tval_to_str(v)` | `(string)` → **新引用，用完 release 或包 `TV_STR` 返回** |
| `tval_loose_eq(a, b)` / `tval_strict_eq(a, b)` | `==` / `===` |
| `tphp_val_type_name(v)`（`class.h`） | PHP 风格类型名（报错文案用："… must be of type string, int given"） |

### 2.6 抛异常（PHP 函数出错就 throw）

```c
#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)   /* 各 ext 文件的统一本地宏，照抄一行 */

RAISE(vm, "ValueError", "acme\\hello\\divide(): division by zero");
RAISE(vm, "ArgumentCountError", "acme\\hello\\my_fn() expects exactly 1 argument");
```

- `tphp_vm_raise(vm, 类名字符串, fmt, ...)`（`opcode.h`，noreturn）——类名 = PHP 异常 FQCN（内置层级 `Exception`/`Error`/`TypeError`/`ValueError`/`RuntimeException`… 用户类也认）。
- 抛出即**不返回**；失败路径上已建的资源先 `tval_release` 再 raise（或先释放再 raise）。
- **返回值协议 ≠ 异常**的场景（如 `preg_*` 运行期失败返回 false）：返回 `TV_FALSE` 即可，不 raise——按你要封装的 PHP 函数的原始语义来。

### 2.7 封装类（原生方法）

方法原型（注意多一个 `self`）：

```c
typedef TVal (*NativeMethodFn)(VM *vm, void *self, TVal *args, int n);
/* self = TObj * 实例 */
```

| 要做的事 | 用什么 |
|---|---|
| 读属性 | `int s = tphp_class_find_prop(o->cls, "count"); TVal v = o->props[s]; tval_addref(v);` |
| 写属性 | `tval_addref(newv); tval_release(&o->props[s]); o->props[s] = newv;` |
| 调 PHP/其它方法 | 现有样例见 `ext/standard`；v1 常见做法是把逻辑放方法本体 |
| 建类骨架 | 照抄脚手架桩或 [src/exclass.c](../src/exclass.c)：`tm_malloc`+`memset` → `tstr_intern` 名字 → `kind = CK_CLASS` → **magic 索引全部置 -1** → `cls->m_ctor = 0` 后**第一个** `add_native_method("__construct", ...)` → 挂 `TProp` 数组 → `tphp_class_register_compile(cls)` |

`add_native_method` 不是导出 API——exclass.c / 脚手架桩各自的 static 助手，照抄约 10 行（追加 `TMethod` 数组、`m->name = tstr_new(...)`、`m->native = fn`）。

### 2.8 注册面（init 里调用，三面）

```c
/* pkg.h —— 名字带 FQ 命名空间，C 字面量里写 \\  */
tphp_ext_reg_const_str("acme\\hello\\VERSION", "0.1.0");
tphp_ext_reg_const_int("acme\\hello\\MAX", 1024);
tphp_ext_reg_fn("acme\\hello\\hello", hello);          /* BuiltinFn，见 §3-R1 */

/* 类：无注册口，构造后直接入注册表（§2.7） */
tphp_class_register_compile(cls);
```

时机：`init` 由 `tphp_run_ext_inits()` 在**首个脚本编译之前**执行一次（进程级，注册常驻）。查询侧引擎已挂钩：`resolve_fn` 内置表未命中后找 ext 函数（含命名空间裸名回退）、`op_fetch_const` 认 ext 常量、`defined()` 认账、`define()` 不可覆盖预定义。

## 3. 配方：四个完整骨架

### R1 函数：带参带返回（对应 §0 的 hello）

```c
#include "pkg.h"      /* tphp_ext_reg_fn */
#include "value.h"    /* TVal / TV_* / tstr_* */
#include "opcode.h"   /* VM */
#include "builtin.h"  /* need_str */
#include <stdio.h>
#include <string.h>
#define RAISE(vm, cls, ...) tphp_vm_raise(vm, cls, __VA_ARGS__)

static TVal hello(VM *vm, TVal *args, int n) {
    if (n < 1)
        RAISE(vm, "ArgumentCountError", "acme\\hello\\hello() expects exactly 1 argument");
    TStr *who = need_str(vm, &args[0], "acme\\hello\\hello", "who");   /* TypeError */
    char buf[512];
    snprintf(buf, sizeof buf, "hello, %s", tstr_val(who));
    return TV_STR(tstr_new(buf, strlen(buf)));                          /* 所有权转移给 VM */
}

void tphp_ext_acme_hello_init(void) {
    tphp_ext_reg_fn("acme\\hello\\hello", hello);
}
```

### R2 函数：返回数组

```c
static TVal stats(VM *vm, TVal *args, int n) {
    (void)vm; (void)args;
    TArr *a = tarr_new();
    tarr_append(a, TV_INT(7));                       /* append 搬入：交出后不再 release */
    tarr_append(a, TV_STR(tstr_new("ok", 2)));
    tarr_set(a, TV_INT(10), TV_INT(1));              /* $a[10] = 1 */
    return TV_ARR(a);
}
```

（真实样例：`ext/json/json.c` 解码建数组的写法——`tarr_new` → 构造元素 → `tarr_append` → 整体 `TV_ARR` 返回；错误路径用 `tval_release(&（TVal){ .u.p = a, .t = T_ARR })` 销毁半成品。）

### R3 类：属性 + 方法（脚手架桩同款 + demo 带成员版）

```c
static TVal nm_counter_add(VM *vm, void *self, TVal *args, int n) {
    TObj *o = (TObj *)self;
    int64_t step = n >= 1 ? need_int(vm, &args[0], "…::add", "n") : 1;   /* 默认参数 = n 判缺 */
    int s = tphp_class_find_prop(o->cls, "count");
    int64_t cur = (o->props[s].t == T_INT) ? o->props[s].u.i : 0;
    tval_release(&o->props[s]);
    o->props[s] = TV_INT(cur + step);
    return o->props[s];                 /* 属性是 T_INT 标量：可直接返回；堆值需 addref */
}
/* 建类/注册：照抄 §2.7；脚手架生成的 src/hello.c 即最小可编译版本 */
```

### R4 常量

```c
tphp_ext_reg_const_str("acme\\hello\\VERSION", "0.1.0");   /* 字符串 */
tphp_ext_reg_const_int("acme\\hello\\BASE", 1024);         /* 整数 */
/* PHP：echo acme\hello\VERSION;  ns 内可省前缀回退；defined() = true */
```

## 4. 包结构与 manifest 契约

```text
hello/
├── mod.json            # type=ext + ext{} 编译契约
├── src/hello.c         # 你的封装（§1 第 3-4 步）
├── include/hello.h
├── README.md           # Next 三步动线（脚手架生成）
├── .gitignore          # build/ *.o *.obj tphp.next tphp.old
└── examples/usage.php  # 消费示例（与自注册桩配套，开箱可跑）
```

```jsonc
{
  "name": "acme/hello", "version": "0.1.0",
  "type": "ext",                          // 必填，安装时强校验（双段名，单段拒）
  "ext": {
    "sources": "src",                     // 必填：目录/目录数组，递归仅 *.c，排除 build/，禁 ..
    "include": ["include/"],              // 包内 -I
    "defines": ["FOO=1"], "cflags": [], "libs": [], "ldflags": [],
    "os": { "windows": { "libs": ["ws2_32"] },   // 与默认数组 append（先默认后 os）
            "linux":   { "libs": ["m", "pthread"] } },
    "tphp_abi": "0.0.2",                  // 与 tphp -v 比对，不匹配拒编（缺则 warning）
    "init": "tphp_ext_acme_hello_init"    // 链入后调用；缺省用约定名 tphp_ext_<v>_<p>_init
  }
}
```

**禁写** `autoload` / PHP `deps` / `scripts`（直接 error）。`cflags/ldflags` 复杂条件用 C 内 `#ifdef`，`os` 只管命令行。

## 5. 链入、换像与命令（2026-11-12 拍板）

| 行为 | 说明 |
|---|---|
| **add 恒编译重链** | 不看 `.built` 跳过——改了 C 再 `add` 立即出新 tphp |
| 换像（Windows） | `tphp.exe → .old`、`tphp.next → .exe`；**当前进程仍旧镜像，下次运行即新**；启动顺带删 `.old` |
| `cc` 选择 | `os[当前].cc` → 顶层 `cc` → `TPHP_EXT_CC` → `<tphp_root>/tcc/tcc` → PATH clang → gcc |
| `ext --relink` | 手换二进制后用现存 `ext/*/` 全量重链 |
| `ext --remove/--update/--list/--add`（无 source=sync） | 卸载重链 / 按 url 重下（force 跳 integrity）/ 源码×已链入×项目声明 三列 / 按 `ext[]` 补齐 |
| 项目集成 | 项目 `mod.json` 只写 `ext[]{name,url,ref}`；`mod.lock` 独立 `ext` 分栏 + integrity（排除 `build/`），篡改 fail-closed |

## 6. 排错速查

| 现象 | 原因/处置 |
|---|---|
| `target directory already exists` | `--force` 覆盖（**破坏性整目录重建**，仅 ext 有此开关） |
| `invalid extension name … (need vendor/package)` | 单段名 → `acme/hello` |
| `not an ext package (type …)` | 源包 `type != "ext"` |
| `tphp_abi` 不匹配拒编 | 改包声明或升 tphp |
| `init` 符号链接后须存在 | `ext.init` 与 C 函数名不一致 |
| `integrity …` 拒绝 | 安装目录被改动与 lock 快照不符 → `--update` 重装 |
| `link failed` + unresolved | 缺 CRT/系统库 → 补 `ext{}.libs/ldflags`（`os` 分平台）；确认 cc 与编 tphp 的工具链同源（`TPHP_EXT_CC`） |
| 调用返回后崩溃/乱码 | **引用没配对**：借用值（属性/`tarr_get`/`args`）存下来前忘 `tval_addref`，或对搬入数组的元素多 release 了——回 §2.2/§2.3 |
| `add` 完没生效 | 换像下次启动生效——用**新起的**进程跑；`ext --list` 看 built-in 列 |
| 生成 C 里命名空间不对 | C 字面量写 `\\`（`"acme\\hello\\fn"`）；PHP 侧声明/调用写单 `\` |
| 类方法全没生效 | magic 索引忘置 -1、或 `m_ctor` 没在首位注册 `__construct`——回 §2.7 |

## 7. 测试

- 结构/契约锁在主套件：`tests/pkg/test-pkg-new-ext.php`（脚手架六件 + notinstall 落点锁）、`tests/pkg/test-pkg-ext.php`（add/写回/同步/lock/integrity/remove 全流程）；`powershell -ExecutionPolicy Bypass -File tests/run.ps1`（基线 242×0）。
- **封装行为的运行时验证**：relink 后二次启动才生效，单进程 test runner 无法自验（ext-packages §2.4 记档，待补集成测试）——手动三步即 §0，用 `examples/usage.php` 或自写脚本断言输出。
