# tphp ext · C 扩展包管理

状态：**切片 1–8 全部完成（2026-11-11）**——设计定稿 + `new --ext` + TMod 双形态 + `tphp ext` 命令组 + 编译链入 + lock `ext` 分栏/integrity + 测试（`test-pkg-ext`）+ 文档收口。
权威规格与拍板决策见本文 §10；进度日志 `memory/2026-11-11.md`。
上级索引：[../PLAN.md](../PLAN.md)；PHP 包管理见 [pkg-manager.md](pkg-manager.md)（二者边界见 §1）。
CI 导出含 tcc/libtphp.a/include 见 [../doc/build.md](../doc/build.md) CI 节。

## 0. 动机与定位

CI artifact 导出 **tphp + libtphp.a + include/ + tcc/**，目的是：用户能写 C 扩展并**二次编译**进 tphp，无需完整 TinyPHP.rt 源码树（有源码也可走 `build.*` 全量重编）。

| | PHP 包（`deps` / `vendor/`） | **C 扩展（`ext[]` / 运行时旁 `ext/`）** |
|---|---|---|
| 内容 | PHP 源码 + autoload | **仅 C/ObjC**（`.c`/`.m` + 头） |
| 落盘 | 项目 `vendor/` | **`<tphp_root>/ext/<name>/`**（与 tphp 二进制同级） |
| 生效 | 运行期 require/autoload | **编译 + 链入当前 tphp 后**才生效 |
| 命令 | `tphp add/remove/install` | `tphp ext --add/--remove/--update/--list/--relink` |
| 脚手架 | `tphp new` / `new --lib` | **`tphp new <name> --ext`**（不设 `ext --new`） |

**铁律**：ext 与 PHP 包**绝不混装**（不进 `vendor/`、不进 autoload）。仍**无**运行期 `.so` 插件——扩展开局编进二进制（与 PLAN/05 一致）。

**tcc 与 libtphp.a**：同平台同架构下，tcc 可直接链 clang 产出的 `libtphp.a`（artifact 内三件套即为此设计）。跨架构不行；系统库由 `os.libs` 补齐。

## 1. 目录模型

```text
<tphp_root>/                 ← tphp 可执行文件所在目录（= artifact 解压根）
├── tphp(.exe)
├── libtphp.a
├── include/
├── tcc/                     ← 可选（Fetch tccbin）
└── ext/                     ← C 扩展唯一落点
    ├── .built               ← 已链入当前二进制的清单（生成物）
    └── acme/foo/
        ├── mod.json         ← 扩展包清单（§2）
        ├── src/foo.c
        ├── include/foo.h
        └── build/           ← .o / link 日志（可清；不进 integrity）

my-project/                  ← 用户 PHP 项目（只声明，不存 C 源）
├── mod.json                 ← deps + ext[]（§3）
├── mod.lock                 ← deps 与 ext 分栏（§6）
└── vendor/                  ← 仅 PHP 包
```

- **`tphp_root`** = `tphp` 可执行文件目录（`tphp_pkg_set_self_exe` 已有路径，取 dirname）。
- 扩展源码**不在**项目树内 → 与 TinyPHP.rt 自身 `ext/`（builtin/net/…）、与用户 `vendor/` 均不撞名。

## 2. 扩展包 `mod.json`（编译契约）

由 `tphp new <name> --ext` 生成；`scripts` **可选**（**2026-11-16 翻案 §10.3**，原「扩展不带生命周期钩子」废止——六键时机映射见 §7）。

```jsonc
{
  "name": "acme/foo",              // 必填，= 目录名，双段 ID
  "version": "0.1.0",              // 必填
  "type": "ext",                   // 必填，load 时强校验

  "ext": {
    "sources": "src",              // 必填：目录（string）或目录数组；递归收集 *.c/*.m
    "include": ["include/"],       // 可选：包内 -I
    "defines": ["FOO=1"],          // 可选：-D
    "cflags": [],                  // 可选：默认平台 cflags
    "libs": [],                    // 可选：默认 -l
    "ldflags": [],                 // 可选；token 支持 $PKG 前缀（§2.2）

    "os": {                        // 可选：按平台覆盖/追加（§2.2）
      "windows": { "sources": ["backend/win32"], "libs": ["ws2_32"], "cflags": [], "ldflags": [], "cc": null },
      "linux":   { "libs": ["m", "pthread"] },
      "darwin":  { "ldflags": ["-framework", "CoreFoundation"] }
    },

    "tphp_abi": "0.0.2",           // 建议：与 tphp -v 版本比对，不匹配拒编
    "init": "tphp_ext_acme_foo_init" // 可选：链入后调用的符号；缺省用约定名
  },

  "author": "", "description": "", "license": ""
}
```

### 2.1 字段规则（防乱套）

| 规则 | 说明 |
|---|---|
| `type` 必须是 `"ext"` | 非 ext → 拒绝当扩展安装 |
| `sources` 仅目录 + 收 `*.c`/`*.m` | 递归；忽略 `.h/.o` 等；**排除 `build/`**；禁 `..`/绝对路径 |
| 禁止 `autoload`、PHP `deps` | 写了 → **error**（尽早暴露误放）；`scripts` 允许（§7） |
| `tphp_abi` | 有则与当前版本比对；无则 warning 放行 |
| `init` 符号 | 链接后须存在；缺 → 编译失败并提示 |
| `cflags/ldflags` 透传 | 文档标风险；复杂条件用 C 内 `#ifdef`，`os` 只管命令行 |

### 2.2 `os` 合并语义

- 键：`windows` | `linux` | `darwin`（mac 两 CI job 同 `darwin`）。
- **`sources` / `cflags` / `libs` / `ldflags`：默认数组与平台数组 append（先 default 再 os）**。
  `os.<平台>.sources` 为分平台追加源目录（同递归收集规则），典型用途 = 窗口后端
  这类「同一套清单、各平台编不同文件」的场景（tui：win32/x11/cocoa 三选一）；
  非当前平台的 `os.<平台>` 目录不扫描、缺失也不报错。
- `cc`：`os[当前].cc` → 顶层（若有）→ 探测链：`TPHP_EXT_CC` → `<tphp_root>/tcc/tcc(.exe)` → PATH 上 clang → gcc。
- **`$PKG` 路径前缀（libs/ldflags）**：token 以 `$PKG` 开头 → 链接时展开为包目录**绝对路径**（正斜杠；含空格自动加引号）。用途：`scripts` 把 **C++ 等外部语言**编译成中间产物落包内 `build/`（tphp 要求 C，webview/GUI 这类 C++ 库走此路），再以 `"$PKG/build/xxx.o"` 参与本次链接。`$PKG` 与 `os.ldflags` 同样生效；只做前缀展开，不做通配。
- 未知 os 键 → warning 忽略。

### 2.3 `--ext` 脚手架生成物

> **2026-11-12 拍板修正**：`new --ext` 创建的是 ext **包** → 落**当前目录**（cwd，与 `--app`/`--lib` 同侧，目录取**最后一段** `pkg`，同 `--lib` 不嵌 vendor）；`<tphp_root>/ext/` 仅是 `ext --add/sync` 的**安装落盘**，脚手架随后由 `ext --add <path>` 链入。

```text
./foo/                             ← cwd（CLI：tphp new acme/foo --ext → ./foo/）
├── mod.json          // 上表，init=tphp_ext_acme_foo_init，tphp_abi=当前版本
├── src/foo.c         // 约定 init/abi 符号 + 最小示例
└── include/foo.h
```

- `name` 必须双段 `vendor/pkg`；与 `--lib` 互斥。
- 目录已存在 → 拒绝（`--force` 才覆盖脚手架）。
- **不写**任何项目 `mod.json`。

约定符号（脚手架与编译共用）：

```c
void tphp_ext_acme_foo_init(void);   // 链入后 registry 调用
int  tphp_ext_acme_foo_abi(void);    // 可选
```

`name` 中 `/`、`-` → `_`：`tphp_ext_<vendor>_<pkg>_init`。

### 2.4 注册 API v1（2026-11-12 落地）

`init` 内可用（进程生命周期，先于首脚本编译——`main.c` `tphp_run_ext_inits` 在 parse 前）：

| 面 | API | 查询侧（引擎挂钩） |
|---|---|---|
| 常量 | `tphp_ext_reg_const_str/int("Ns\\NAME", v)`（pkg.h） | `op_fetch_const` 全名 +（unqualified 时）裸名旁路；`defined()` 认账；`define()` 视为预定义不可覆盖 |
| 函数 | `tphp_ext_reg_fn("Ns\\fn", BuiltinFn)`（pkg.h） | `resolve_fn` 内置表未命中后旁路，同 builtin 调用协议（含 ns bare 重试） |
| 类 | 无需注册口：按 `src/exclass.c` 蓝本构造 `TClass`（`TMethod.native` 原生方法、`props` 槽位）+ `tphp_class_register_compile()`（class.h） | 类解析原生注册表路径（编译期 A_NEW 存在性检查亦认） |

- 实现居 `ext/pkg/pkg.c`（与 `tphp_run_ext_inits` 同基座），查询口声明在 `src/opcode.h`；公开面随 `build/include` 分发（pkg.h / class.h / opcode.h / value.h / builtin.h 全在）。
- 端到端示例（库外手测）：demo 扩展包 `src/demo.c` 注册 `Tphp\Demo\VERSION/BASE/hello/Counter` → `ext --add` 链入 → `demo.php` 消费（C 段输出 `C const/C fn/C class`），**242 套件在链入态全绿**。
- 套件内暂无自动化锁：注册进 exe 需 relink 后**下次启动**生效，单进程 test runner 无法自验——后续切片可补 relink+二次启动集成测试。

## 3. 项目侧 `mod.json`：`ext[]`

```jsonc
{
  "name": "my-app",
  "deps": { "acme/utils": { "url": "…", "ref": "v1" } },   // PHP → vendor/
  "ext": [                                                 // C → <tphp_root>/ext/
    { "name": "acme/foo", "url": "https://…/foo-ext.zip", "ref": "v1.0.0" },
    { "name": "acme/bar", "url": "../local/bar-ext", "ref": "" }
  ]
}
```

- `url`/`ref` 语义与 `deps` 相同（GitHub / zip / 本地路径）。
- 项目**不**写 `sources/os` 等编译细节——那些只在扩展包清单里。
- 解析：`ext` 为 **数组** → 项目依赖表；扩展包清单里 `type=ext` 且 `ext` 为 **对象** → 编译契约。同一键、两种形状，靠 `type`/上下文区分。

## 4. 命令

```text
tphp new <name> --ext              # 脚手架 → ./<pkg>/（cwd 最后一段；add 后才落 <tphp_root>/ext/）

tphp ext --add <source> [ref]      # 装源码 + 恒编译重链（11-12 拍板，见下）
tphp ext --add                     # 无 source：按当前项目 ext[] 补齐（= sync）
tphp ext --remove <name>           # 删 <tphp_root>/ext/<name>/
tphp ext --update [name]           # 按 url/ref 重下；变了则重编；省略 name=全部
tphp ext --list                    # 源码 × 二进制已链入 × 项目声明 对照
tphp ext --relink                  # 用现存 ext/*/ 全量重链（remove 后清 registry）
```

### 4.1 `--add` 双模式（用户拍板）

| 上下文 | 行为 |
|---|---|
| **cwd 有 `mod.json`（项目内）** | 落盘 `<tphp_root>/ext/<name>/` → 编译链入 → **把 `{name,url,ref}` 追加进项目 `ext[]`**（已有同 name 则更新 ref/url） |
| **非项目** | 只落盘 + 编译链入，**不写**任何项目文件 |

### 4.2 项目缺 ext 时（用户拍板：「没有就去下载然后编译」）

`--add` 无 source，或显式按 `ext[]` 对齐：

```text
对项目 ext[] 每一项：
  <tphp_root>/ext/<name> 不存在 → 按 url/ref 下载解压落盘
  二进制未链入 → 编译 + 重链
  已链入且源码在 → skip
```

即 **sync 并入第一批**（不做「后置」）。

### 4.3 「是否已链入」检测

- 生成物 **`<tphp_root>/ext/.built`**：每行 `name version`，**仅在本机成功重链后重写**（扫描现存 `ext/*/mod.json`）。
- `has(name)` = 读 `.built`；`--list` 三列：source / built-in / 项目是否声明。
- 备选（后置）：二进制内 `tphp_ext_registry[]` 强符号表——避免 sidecar 与手工换二进制脱节；v1 用 sidecar + 文档说明「手换 tphp 后请 `--relink` 或重写 .built」。

## 5. 编译与链接（第一批就做，用户拍板「一起做」）

```text
1. 读扩展包 mod.json（type/ext 字段/abi）
2. 展开 sources 目录 + `os[当前].sources` → 递归 *.c/*.m（排除 build/）
3. 合并 cflags/libs/ldflags（default + os[当前]）
4. 逐个 .c → <pkg>/build/<base>.o
     cc -c -I<tphp_root>/include -I<pkg>/include… [-D…] [cflags] src.c -o build/base.o
5. 链接：
     cc *.o <tphp_root>/libtphp.a [libs…] [ldflags…] -o tphp.next
   - Windows 默认 libs 对齐 build.cmd 系统库表；linux/darwin 见 os 默认
   - tcc 或 clang 均可；同平台链 clang 的 libtphp.a 为预期路径
6. 原子替换 tphp：
   - Unix：rename tphp.next → tphp
   - Windows：运行中 exe → 先 rename 旧为 tphp.old，再 tphp.next → tphp.exe，
     下次启动删 tphp.old
7. 失败：保留旧二进制，打印完整命令，exit ≠ 0
8. 成功：重写 ext/.built
```

**ABI**：`tphp_abi` 与当前 `tphp -v` 不匹配 → 拒编（文档：必须用同一 release 的 artifact 三件套）。

## 6. lock 分栏（第一批，用户拍板「一起做」）

`mod.lock` 在既有 `deps` 之外增加 **`ext`** 段（结构同 deps：name/url/ref + 可选 integrity 对 `<tphp_root>/ext/<name>/` 逐文件 sha1，**排除 `build/`**）。

- `tphp ext --add`（项目内）写 lock。
- integrity 校验点：`ext --add` 覆盖前、（可选）`--relink` 前。
- 与 PHP `deps` 分栏，禁止混表。

## 7. scripts 生命周期钩子（2026-11-16 翻案 §10.3，拍板：六键对齐 lib/app）

扩展包 `mod.json` 可带 `scripts`——**六键、条目形态（string|string[]、`.php` 子进程、shell 需 `TPHP_PKG_SCRIPTS_SHELL=1`）、路径约束（相对包根、禁 `..`/绝对）、env（`TPHP_PKG_*`）、开关（`TPHP_PKG_SCRIPTS=0`/`--ignore-scripts`）全部复用 [pkg-manager.md](pkg-manager.md) §2.1.1，不另造机制**。差异 = 时机映射 + postRemove 触发面 + 失败语义按实证（见 ③）：

**① 时机映射（ext 的 install = 落盘 + 编译链入，post 统一在「落盘后、编译前」）**

| 键 | ext 时机 | cwd | 触发 |
|---|---|---|---|
| `preInstall` | 新装落盘前 | 暂存/源目录（与 PHP 包同） | ✓ |
| `postInstall` | 落盘后、**编译链入前** | `<tphp_root>/ext/<name>/` | ✓ |
| `preUpdate` | 覆盖已有扩展前 | **旧** `<tphp_root>/ext/<name>/` | ✓ |
| `postUpdate` | 覆盖后、**重编前** | 新 `<tphp_root>/ext/<name>/` | ✓ |
| `preRemove` | 删除前 | 即将删除的 `<tphp_root>/ext/<name>/` | ✓ |
| `postRemove` | — | — | **✗ 不触发**（见 ②） |

- action 判定与 PHP 同：按目标目录已存在（`ext --add` 已有 = update、`--update` = update）。
- post 落在编译前 ⇒ **生成 `.c`/头文件可直接参与本次编译**（codegen 场景），与 PHP 包「post=落盘后」心智对齐；配合 libs/ldflags 的 **`$PKG` 前缀**（§2.2），脚本也能产出**二进制中间产物**（`build/*.o`、静态库）直接进本次链接——C++ 依赖场景的固定打法：C 扩展面走 ext 正常编译，C++ 库由 scripts 用 `clang++`/`g++` 编成 `.o`，`ldflags: ["$PKG/build/xxx.o"]` 链入。
- 编译/链接失败的语义不归钩子管：照旧保留旧二进制、exit ≠ 0（§5.7）。

**② postRemove 解析保留、不触发（2026-11-16 拍板）**：包删除后脚本文件随包删除，`.php` 条目无处可跑——**与 PHP 包侧行为完全一致**（PHP 的包声明 `postRemove` 也从不触发，该键在 pkg-manager §2.1.1 仅根项目可用；ext 无根面 → 域内无触发面）。清理类逻辑放 `preRemove`。六键全部解析接受、不报错，实际触发 5 键。

**③ 失败语义按实证镜像 lib/app 现状（2026-11-16 拍板，与 §2.1.1 文字有出入处以此为准）**：
- `pre*` 脚本**退出非 0** → **PHP Warning + 继续**（实测 lib/app 现状：add 照常完成、exit 0；§2.1.1「pre 非 0 → 中止」文字对退出非 0 不成立——lib/app 5 个 pre 调用点均放行 rc=3）。仅**配置类错误**（脚本缺失 / 路径逃逸 / spawn 失败 → rc 1/2）→ **中止**，不改 ext[]/lock/目录。
- `post*` 脚本退出非 0 → 警告 + **命令退出码非 0**、不回滚（builtin 抛 RuntimeException、CLI 走非 0 出口）；post 配置类错误 → 仅警告（镜像 `install_from_src`）。

**④ 仅扩展包自身**：项目 `mod.json` 的 `ext[]` 数组**不跑**钩子（维持原立场——声明表不是包，无生命周期可言；`TPHP_PKG_ROOT` 面在 ext 域不存在）。

- 触发命令：`ext --add`（= install/update，按目标是否已有）、`--update`、`--remove`、无参 sync 补装缺失项；`--relink`/已存在 skip **不跑**（无落盘/删除事件）。
- `new --ext` 脚手架不生成 scripts 段（用到再写）。
- 安全同 §2.1.1：装包即信任；`tphp build` 打包：`ext/` 源码默认不进 zip（已编进 exe）；`--with` 可显式带。
- `PLAN/05`：仍声明「无动态插件」；ext = 源码级并入。

## 8. 风险

1. **换二进制 = 换扩展集**：多项目共用一份 tphp 时，`.built` 是全局的——文档写清。
2. **Windows 运行中替换 exe**：old/new 双 rename；失败则打印手动步骤。
3. **tcc × clang libtphp.a**：同平台预期可用；符号/子系统差异时用 `TPHP_EXT_CC=clang` 回退。
4. **`libtphp.a` 含 main.o**：重链 = 整颗 tphp 再出一份 + 扩展 .o——预期行为。
5. **cflags 任意透传**：可破坏可重定位；CI 不测任意 flags。

## 9. 实现切片（开工顺序）

1. 规格入库（本文）+ `PLAN/pkg-manager.md` 指针 + `doc/package-manager.md` 摘要  
2. `TMod`：项目 `ext[]` 解析/写出；扩展包 `type=ext` + `ext{}` 解析（含 `os`）  
3. `tphp_root()` + `tphp new --ext` 脚手架（root 供 add/sync；脚手架 2026-11-12 改落 cwd）  
4. `tphp ext --add/--remove/--update/--list/--relink`（含项目内写回 `ext[]`、无参 sync）  
5. 编译链接 + `.built` + Windows 替换  
6. lock `ext` 分栏 + integrity（可与 4 同 PR）  
7. 测试：脚手架结构 / 非法 type 拒 / 项目写回 / 非项目不写 / 已链入 skip / 缺源码报错 / `os` 合并单测（若可）  
8. 文档 + memory 记账  

## 10. 已拍板决策清单（防回退）

| # | 决策 |
|---|---|
| 1 | 源码根 = **`<tphp_root>/ext/<name>/`** |
| 2 | 脚手架 = **`tphp new <name> --ext`**（无 `ext --new`） |
| 3 | ~~扩展包 **无 `scripts`**~~ **2026-11-16 翻案（用户明示）**：六键对齐 lib/app、post=落盘后编译前、仅扩展包自身——现行规格 §7 |
| 4 | `sources` = **目录**（`*.c`；2026-11-14 扩展：收 `*.m` + `os.<平台>.sources` 分平台追加，tui 三平台窗口后端实证） |
| 5 | **`os` 分平台**编译/链接指令；flags **append** 合并 |
| 6 | **编译链入 + sync + lock 分栏同属第一批** |
| 7 | 项目内 `--add` **写回** `ext[]`；非项目只装 |
| 8 | 项目缺 ext → **自动下载 + 编译** |
| 9 | ext 与 PHP 包目录/命令/lock **全面分离** |
| 10 | 仍无运行期动态插件 |
