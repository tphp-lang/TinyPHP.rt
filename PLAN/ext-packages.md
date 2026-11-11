# tphp ext · C 扩展包管理（设计稿，未实现）

状态：**设计定稿 2026-11-10（用户逐条拍板），代码未动**。开工时以本文为唯一规格。
上级索引：[../PLAN.md](../PLAN.md)；PHP 包管理见 [pkg-manager.md](pkg-manager.md)（二者边界见 §1）。
CI 导出含 tcc/libtphp.a/include 见 [../doc/build.md](../doc/build.md) CI 节。

## 0. 动机与定位

CI artifact 导出 **tphp + libtphp.a + include/ + tcc/**，目的是：用户能写 C 扩展并**二次编译**进 tphp，无需完整 TinyPHP.rt 源码树（有源码也可走 `build.*` 全量重编）。

| | PHP 包（`deps` / `vendor/`） | **C 扩展（`ext[]` / 运行时旁 `ext/`）** |
|---|---|---|
| 内容 | PHP 源码 + autoload | **仅 C**（`.c` + 头） |
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

由 `tphp new <name> --ext` 生成；**无 `scripts`**（用户拍板：扩展不带生命周期钩子）。

```jsonc
{
  "name": "acme/foo",              // 必填，= 目录名，双段 ID
  "version": "0.1.0",              // 必填
  "type": "ext",                   // 必填，load 时强校验

  "ext": {
    "sources": "src",              // 必填：目录（string）或目录数组；递归收集 *.c
    "include": ["include/"],       // 可选：包内 -I
    "defines": ["FOO=1"],          // 可选：-D
    "cflags": [],                  // 可选：默认平台 cflags
    "libs": [],                    // 可选：默认 -l
    "ldflags": [],                 // 可选

    "os": {                        // 可选：按平台覆盖/追加（§2.2）
      "windows": { "libs": ["ws2_32"], "cflags": [], "ldflags": [], "cc": null },
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
| `sources` 仅目录 + 仅收 `*.c` | 递归；忽略 `.h/.o` 等；**排除 `build/`**；禁 `..`/绝对路径 |
| 禁止 `autoload`、PHP `deps`、`scripts` | 写了 → **error**（尽早暴露误放） |
| `tphp_abi` | 有则与当前版本比对；无则 warning 放行 |
| `init` 符号 | 链接后须存在；缺 → 编译失败并提示 |
| `cflags/ldflags` 透传 | 文档标风险；复杂条件用 C 内 `#ifdef`，`os` 只管命令行 |

### 2.2 `os` 合并语义

- 键：`windows` | `linux` | `darwin`（mac 两 CI job 同 `darwin`）。
- **`cflags` / `libs` / `ldflags`：默认数组与平台数组 append（先 default 再 os）**。
- `cc`：`os[当前].cc` → 顶层（若有）→ 探测链：`TPHP_EXT_CC` → `<tphp_root>/tcc/tcc(.exe)` → PATH 上 clang → gcc。
- 未知 os 键 → warning 忽略。

### 2.3 `--ext` 脚手架生成物

```text
<tphp_root>/ext/acme/foo/
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
tphp new <name> --ext              # 脚手架 → <tphp_root>/ext/<name>/

tphp ext --add <source> [ref]      # 装源码 + 按需编译（见下）
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
2. 展开 sources 目录 → 递归 *.c（排除 build/）
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

## 7. 与 scripts / 其它

- **扩展包无 scripts**（已拍板）。
- 项目 `ext[]` 也不跑 PHP 包那套 pre/post（无意义）。
- `tphp build` 打包：`ext/` 源码默认**不**进 zip（已编进 exe）；`--with` 可显式带。
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
3. `tphp_root()` + `tphp new --ext` 脚手架  
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
| 3 | 扩展包 **无 `scripts`** |
| 4 | `sources` = **目录**（仅 `*.c`） |
| 5 | **`os` 分平台**编译/链接指令；flags **append** 合并 |
| 6 | **编译链入 + sync + lock 分栏同属第一批** |
| 7 | 项目内 `--add` **写回** `ext[]`；非项目只装 |
| 8 | 项目缺 ext → **自动下载 + 编译** |
| 9 | ext 与 PHP 包目录/命令/lock **全面分离** |
| 10 | 仍无运行期动态插件 |
