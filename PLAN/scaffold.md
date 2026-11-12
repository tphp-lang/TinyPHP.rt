# 脚手架（tphp new）设计

> 上级索引：[../PLAN.md](../PLAN.md)。兄弟文档：[pkg-manager.md](pkg-manager.md)（包管理核心与命令面）、[ext-packages.md](ext-packages.md)（ext 包契约 / 链入 / 注册 API §2.4）、[05-extension-guide.md](05-extension-guide.md)（内置扩展，与本文件的用户侧脚手架不同域）。
> 状态：**现状盘点 + 目标态设计（2026-11-12）**——三形态（app/lib/ext）已实现且可用，但生成物质量不对齐；§4 为目标态，§5 缺口清单待逐项排刀，§5 末两项待拍板。

## 1. 定位与命令面

一个入口三形态（[src/main.c](../src/main.c) `new` 派发，`--ext` 与 `--lib` 互斥）：

```text
tphp new <name>              # 应用：./<name>/ + main.php
tphp new --lib <name>        # 库包：./<pkg>/  + src/tests/autoload
tphp new <name> --ext        # C 扩展包：./<pkg>/ + mod.json ext{} 契约
tphp new ... [--force]       # --force 仅 ext（11-12 拍板维持；覆盖已存在目录=破坏性，见 §5 决策 6）
```

| 形态 | 目录（2026-11-12 拍板后） | name 规则（valid_name） | mod.json type |
|---|---|---|---|
| app | `./<name>`（名即目录） | 单段 `[a-z0-9][a-z0-9_-]*` | `app` |
| lib | `./<pkg>`（最后一条 `/` 后段） | 双段 `<seg>/<seg>` | `lib`（含 psr-4 前缀 = name 大小写段 + `\`） |
| ext | `./<pkg>`（最后一条 `/` 后段，**不嵌 vendor**） | 双段 `<seg>/<seg>`（强制） | `ext`（含 `ext{}` 编译契约） |

**三形态共同契约**（[pkg.c `tphp_pkg_new` / `tphp_pkg_new_ext`](../ext/pkg/pkg.c)）：

- 目标已存在 → 拒（不静默合并）；失败路径 `fs_rm_rf` 全量回滚，不留半成品
- **只创建包，不写项目清单**：cwd 即便有 `mod.json` 也不碰（写回发生在 `add` 阶段，见 pkg-manager/ext-packages）
- 落点恒为 cwd 相对路径——`new` 是"创建一个包给你开发"，安装/落盘区（`<tphp_root>/vendor/`、`<tphp_root>/ext/`）归 `add/sync` 管（11-12 拍板，原 ext 落 `<tphp_root>/ext/` 已推翻）

## 2. 现状生成物契约（2026-11-12 盘点）

| 生成物 | app | lib | ext | 说明 |
|---|---|---|---|---|
| `mod.json` | ✓ | ✓（+psr-4） | ✓（+`ext{}`：sources/include/abi/init） | 全走 TMod+tmod_save 统一格式 |
| 主体代码 | `main.php` | `src/Greeter.php`（ns 按 name 派生） | `src/<pkg>.c`（11-12 起 = **自注册最小三件**，见 §5-5） + `include/<pkg>.h` | ext 的 init 符号 = `tphp_ext_<v>_<p>_init`（`/`、`-`→`_`） |
| `tests/test-smoke.php` | ✗ | ✓ | ✗ | app 无（main.php 即冒烟）；ext 见 §5 决策 5 |
| `README.md` | ✓（含 `tphp run main.php`） | ✓（11-12 补 `tphp test tests`） | **✓**（11-12 缺口 1） | ext：包名+注册三件摘要+Next 三步动线 |
| `.gitignore` | ✓ `vendor/` | ✓ `vendor/` | **✓**（11-12 缺口 1） | ext：`build/`、`*.o`、`*.obj`、`tphp.next`、`tphp.old` |
| `vendor/autoload.php` | ✓（骨架即用） | ✓ | —（无 PHP 代码，正当） | `tphp_pkg_gen_autoload` |
| PHP 消费示例 | — | — | **✓**（11-12 缺口 1+5） | `examples/usage.php`：与 C 桩自注册三件配套，链入即开箱可跑 |

## 3. 拍板史（本文件相关）

| 日期 | 拍板 | 影响 |
|---|---|---|
| 2026-11-10 | 切片 1–8：`new --ext` 初版落 `<tphp_root>/ext/<name>/` | 后被 11-12 推翻 |
| 2026-11-12 | **`new --ext` 创建的是 ext 包**，落 cwd（不是解析器安装区） | `tphp_pkg_new_ext(dir,…)` 加 dir 参数、main.c 派发、§1 落点行 |
| 2026-11-12 | **目录不嵌套**：目录取最后一条 `/` 后段（同 `--lib`） | `Created ext scaffold "t/php/ext" in ext/`；双段名保留在 mod.json |
| 2026-11-12 | 注册 API v1 落地（常量/函数/类三面，见 ext-packages §2.4） | ext 脚手架的"C 桩可注册什么"从待暴露变为可生成（§4.3 选项依赖此） |

## 4. 目标态设计

### 4.1 ext 脚手架质量对齐 app/lib（补齐三个不对齐点）

生成物在现有三件套之上增加：

1. **`README.md`**：包名首行 + 「下一步」动线（三步命令，对应实测流程）：

   ```bash
   tphp ext --add <本包路径>     # 编译并链入当前 tphp（重链，下次启动生效）
   tphp run examples/usage.php   # 消费注册的常量/函数/类
   tphp ext --remove <name>      # 卸载恢复基座
   ```

   首段指针 → `PLAN/ext-packages.md` §2.3（契约）/ §2.4（注册 API）。
2. **`.gitignore`**：`build/`、`*.o`、`*.obj`、`tphp.next`、`tphp.old`（ext 编译链在包内 `build/` 落 `.o`，Windows 重链出 `*.next/*.old`）。
3. **`examples/usage.php`**：PHP 侧消费模板——对应注册 API 三面各一行断言式 echo（常量直读、`Ns\fn()` 调用、`new Klass()`+方法），命名空间按包名派生（`ns_prefix_from_name` 同源），**生成即与 C 桩内容配套**（配套关系见 §5 决策 5/6 之前的选项 A/B）。

### 4.2 app / lib

现状即基线，不动（README/.gitignore/autoloader/冒烟已对齐 §4.1 的质量线）。

### 4.3 动线统一原则

三形态 README 首屏都必须回答"**建好了，然后跑哪条命令**"：app = `tphp run main.php`；lib = `tphp test tests`（现有 README 尚未写运行命令，随 4.1 一并补）；ext = 上表三步。

## 5. 缺口清单与待拍板

| # | 项 | 归属 | 状态 |
|---|---|---|---|
| 1 | ext README + `.gitignore` + `examples/usage.php`（§4.1）；lib README 补运行命令（§4.3） | 脚手架实现 | **✓ 已实施（2026-11-12）**：EXT_README_TMPL / EXT_GITIGNORE / EXT_USAGE_TMPL 落 pkg.c；生成六件实测 |
| 2 | [test-pkg-new-ext](../tests/pkg/test-pkg-new-ext.php) 结构 EXPECT 随 1 扩（README/gitignore/examples 三行） | 测试锁 | **✓ 已实施**：`$need` 扩三件走既有 `files-ok`；242×0 |
| 3 | relink + 二次启动的注册集成测试（ext-packages §2.4 记档缺口：单进程 runner 无法自验） | ext 测试 | 后续切片 |
| 4 | 三形态生成物矩阵已有测试：new-app / new-lib / new-ext（6 EXPECT 含 notinstall 拍板锁） | 测试锁 | ✓ 242 基线 |
| 5 | ext C 桩「自注册最小三件」vs 空桩 | 决策 | **已拍板（2026-11-12）：自注册最小三件** → **✓ 已实施**：EXT_C_TMPL 重写（`<ns>\VERSION` const + `<ns>\hello()` fn + `<ns>\Example` 空类，exclass 蓝本最小子集），E2E 三行实测（`const: 0.1.0` / `fn: hello, world (from acme/hello)` / `class: Example ok`） |
| 6 | `--force` 是否统一到三形态 | 决策 | **已拍板（2026-11-12）：维持仅 ext**——app/lib 目录是真实开发起点，覆盖语义弊大于利（建过的应用/库永远拒 = 不会误删开发代码）；ext 保留"改乱重置"场景，README/帮助注明破坏性 |

## 6. 相关测试与基线

- `tests/pkg/test-pkg-new-{app,lib,ext}.php`（ext 含 11-12 二拍的 `notinstall` 锁：旧落点 `build/ext/<vendor>/` 不得出现）
- 全量基线 **242×0**（2026-11-12，demo ext 链入态实测）
