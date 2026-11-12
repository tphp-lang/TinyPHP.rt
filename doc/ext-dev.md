# C 扩展开发指南（用户侧 ext 包）

> 面向：要给 tphp 写 **C 扩展包**（链入 tphp 二进制）的开发者。
> 权威规格：[../PLAN/ext-packages.md](../PLAN/ext-packages.md)（§2 契约 / §2.4 注册 API / §4 命令 / §5 编译链入 / §6 lock）。本文件是上手教程；结论以规格为准。
> 与相邻文档的分野：**内置**扩展（编进源码树 `ext/` 的贡献者工作）见 [../PLAN/05-extension-guide.md](../PLAN/05-extension-guide.md)；PHP 依赖包走 `deps`/`vendor`，见 [package-manager.md](package-manager.md)；脚手架设计见 [../PLAN/scaffold.md](../PLAN/scaffold.md)。ext 包三条铁律：**不进 `vendor/`、不进 autoload、无 `scripts`**。

## 0. 三十秒跑通

```bash
tphp new acme/hello --ext        # 1. 脚手架 → ./hello/（目录取最后一段，不嵌 vendor）
                                 #    生成：mod.json / src/hello.c / include/hello.h
                                 #          README.md / .gitignore / examples/usage.php
$EDITOR hello/src/hello.c        # 2. 改注册内容（自注册最小三件，见 §2）
tphp ext --add ./hello           # 3. 编译并链入当前 tphp（恒编译重链，见 §4）
tphp run hello/examples/usage.php  #    消费注册的常量/函数/类
```

`add` 完成时新 tphp 已换像：**当前进程仍旧镜像，下一条命令即新 tphp**（Windows 换像机制，§4）。

## 1. 包结构与 manifest 契约

```text
hello/
├── mod.json            # type=ext + ext{} 编译契约
├── src/hello.c         # 你的代码（init 入口 + 注册内容）
├── include/hello.h
├── README.md           # 脚手架生成（含 Next 三步）
├── .gitignore          # build/ *.o *.obj tphp.next tphp.old
└── examples/usage.php  # 消费示例，链入即开箱可跑
```

`mod.json` 关键字段（全表见 PLAN/ext-packages §2）：

```jsonc
{
  "name": "acme/hello",          // 必填，双段 vendor/package（单段拒绝）
  "version": "0.1.0",            // 必填
  "type": "ext",                 // 必填，安装时强校验（非 ext → 拒）
  "ext": {
    "sources": "src",            // 必填：目录（或目录数组），递归只收 *.c，排除 build/，禁 ..
    "include": ["include/"],     // 包内 -I
    "defines": ["FOO=1"],        // -D
    "cflags": [], "libs": [], "ldflags": [],
    "os": {                      // 按平台 append（先默认后 os）；cc 选择见 §4
      "windows": { "libs": ["ws2_32"] },
      "linux":   { "libs": ["m", "pthread"] }
    },
    "tphp_abi": "0.0.2",         // 建议：与 tphp -v 比对，不匹配拒编（缺则 warning 放行）
    "init": "tphp_ext_acme_hello_init"  // 链入后调用的符号；缺省用约定名
  }
}
```

**禁止写** `autoload` / PHP `deps` / `scripts`——写了直接 error（尽早暴露误放）。
`init` 符号命名约定：`tphp_ext_<vendor>_<pkg>_init`（`/`、`-` → `_`，脚手架自动算好）。

## 2. 注册 API v1：init 里能注册什么

`init` 在**首个脚本编译之前**执行一次（`main.c` → `tphp_run_ext_inits`），生命周期 = 进程。声明全在脚手架已 include 的头里（`pkg.h` / `class.h` / `opcode.h` / `value.h` / `builtin.h`，随 `build/include` 分发）。三面如下，生成的桩即是最小样例：

### 2.1 常量

```c
tphp_ext_reg_const_str("acme\\hello\\VERSION", "0.1.0");   /* C 字面量：\\ 转义 */
tphp_ext_reg_const_int("acme\\hello\\BASE", 1024);
```

PHP 侧：`acme\hello\VERSION` 全名直读、ns 内未限定写法自动回退、`defined()` 认账、`define()` 视为预定义不可覆盖。**名字用 FQ**（含命名空间段）；全局常量用裸名。

### 2.2 函数

```c
typedef TVal (*BuiltinFn)(VM *vm, TVal *args, int n);   /* opcode.h */

static TVal my_fn(VM *vm, TVal *args, int n) {
    TStr *s = need_str(vm, &args[0], "acme\\hello\\my_fn", "arg"); /* builtin.h：PHP 同款 TypeError */
    /* … 返回 TV_INT / TV_STR(tstr_new(buf, len)) / TV_NULL … */
}

tphp_ext_reg_fn("acme\\hello\\my_fn", my_fn);   /* pkg.h */
```

- 调用协议与内置函数**完全一致**（`resolve_fn` 内置表未命中后旁路，命名空间裸名重试也覆盖）。
- 参数校验用 `need_str/need_int`（strict，文案对齐 PHP "must be of type …"）；不校验则自行判 `args[i].t`。

### 2.3 类（无需注册口，直接构造 TClass）

按 [src/exclass.c](../src/exclass.c)（内部异常类）的蓝本从 C 构造，`tphp_class_register_compile()` 入注册表（先于编译，`A_NEW` 编译期存在性检查亦认）：

```c
static TVal nm_counter_construct(VM *vm, void *self, TVal *args, int n); /* NativeMethodFn */
static TVal nm_counter_add(VM *vm, void *self, TVal *args, int n);

TClass *cls = tm_malloc(sizeof *cls);
memset(cls, 0, sizeof *cls);
cls->name = tstr_intern("acme\\hello\\Counter", strlen("acme\\hello\\Counter"));
cls->kind = CK_CLASS;
cls->m_ctor = cls->m_dtor = /* …全部 magic 索引 */ … = -1;
/* 属性：TProp 数组（name/visibility/defval/slot），挂 cls->props / n_props */
cls->m_ctor = 0;                                        /* 下面第一个注册的就是 __construct */
add_native_method(cls, "__construct", (void *)nm_counter_construct);
add_native_method(cls, "add", (void *)nm_counter_add);
tphp_class_register_compile(cls);
```

- 原生方法签名 `NativeMethodFn = TVal (*)(VM *vm, void *self, TVal *args, int n)`，`self` = `TObj *`；读写属性经 `tphp_class_find_prop(cls, name)` → `o->props[slot]`（+`tval_addref` / `tval_release` 成对）。
- `add_native_method` 不是导出 API——它是 exclass.c / 脚手架桩各自的 static 助手，照抄一份到你的文件即可（追加 `TMethod` 数组 + `m->native` 赋值，约 10 行）。
- 完整可运行样例：手写版带原生方法的 `Counter` 见 demo 包；模板版空类 + 三件见脚手架生成物。

## 3. 编译链入做了什么

`ext --add <源>` 的管线（PLAN §5）：

1. **物化**：源（目录/zip/URL）→ staging 强校验（type=ext、双段名、禁项）→ 覆盖前三查（mod.json/.git/字符）→ 落 `<tphp_root>/ext/<vendor>/<pkg>/`（项目 cwd 有 `mod.json` 时同时写回 `ext[]` 并同步 lock）。
2. **编译**：按 `ext{}` 收 `*.c`（排除 `build/`）→ `cc -c -I<tphp_root>/include -I<pkg>/include … -o *.o`。
3. **链接**：生成 `build/ext/.registry.c`（constructor 挂 `tphp_ext_inits_hook` → 启动时跑全部 `init`）→ 引擎 objs + 包 `.o` + `libtphp.a` → `tphp.next` → **换像**。
4. **记账**：成功后整体重写 `ext/.built`（每行 `name version`）；`ext --list` 三列 = 源码 × 二进制已链入 × 项目声明。

**`cc` 选择**：`ext.os[当前].cc` → 顶层 `cc` → 探测链 `TPHP_EXT_CC` 环境变量 → `<tphp_root>/tcc/tcc(.exe)` → PATH 上 clang → gcc。

## 4. add / relink / 换像行为（2026-11-12 拍板）

| 行为 | 说明 |
|---|---|
| **add 恒编译重链** | 不再看 `.built`「已链入」跳过——改了源码再 `add` 立即编译出新 tphp（重复 add = 一次 relink，秒级） |
| 换像（Windows） | `tphp.exe → tphp.old`、`tphp.next → tphp.exe`；**当前进程仍跑旧镜像**，下次启动即新；启动时顺带删 `.old`（被占用则下次再试） |
| 换像（Unix） | `rename` 直换 |
| `ext --relink` | 用现存 `ext/*/` 全量重链（手换二进制后请 `--relink` 或重写 `.built`） |
| 占用报错 | 换像失败 → `close other tphp processes and retry --relink` |
| `ext --remove <name>` | 删安装目录 + 写回 `ext[]` + 重链基座 |
| `ext --update [name]` | 按 url/ref 重下（force 跳过 integrity）；`sync`（=无参 add）按 `ext[]` 补齐、缺源报错带名 |

## 5. 项目集成与 lock

- 项目 `mod.json` 只以 **`ext[]`** 声明 `{name,url,ref}`（相对 url 按项目解析）；ext 包**不进 deps/vendor/autoload**。
- `mod.lock` 有独立 **`ext` 分栏**（url/ref + `integrity` 对安装目录逐文件 sha1，**排除 `build/`**）。覆盖已装包前校验 integrity，**篡改 → fail-closed 拒绝**；`--update` force 跳过重装恢复。

## 6. 排错速查

| 现象 | 原因/处置 |
|---|---|
| `target directory already exists` | 脚手架目标已存在；`--force` 覆盖（**破坏性：整目录 rm 重生成**，仅 ext 有此开关） |
| `invalid extension name … (need vendor/package)` | 单段名 → 用 `acme/hello` 双段 |
| `not an ext package (type …)` | 源包 `type` 不是 `ext` |
| `tphp_abi` 不匹配拒编 | 源包声明的 ABI 与当前 `tphp -v` 不同；改包或升 tphp |
| `init` 符号链接后须存在 | `ext.init` 与 C 里函数名不一致（约定 `tphp_ext_<v>_<p>_init`） |
| `integrity …` 拒绝 | 安装目录被改过与 lock 快照不符 → `--update` 重装，或排查改动来源 |
| `link failed` + unresolved | 看缺哪组符号：CRT/系统库 → 补 `ext{}.libs/ldflags`（os 分平台）；确认 cc 与编 tphp 的工具链同源（`TPHP_EXT_CC`） |
| 改完 add 没生效 | 确认用**新起的**进程跑（换像下次启动生效）；或 `ext --list` 看 built-in 列 |

## 7. 测试

- 结构/契约锁在主套件：`tests/pkg/test-pkg-new-ext.php`（脚手架六件 + notinstall 拍板锁）、`tests/pkg/test-pkg-ext.php`（add/写回/同步/lock/integrity/remove 全流程）；跑法 `powershell -ExecutionPolicy Bypass -File tests/run.ps1`（基线 242×0）。
- **注册行为的运行时验证目前靠手动 E2E**（relink 后二次启动才生效，单进程 test runner 无法自验——PLAN/ext-packages §2.4 记档，后续补 relink+二次启动集成测试）。手测三步见 §0。
