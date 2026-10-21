# 包管理

`tphp` 内置包管理，无需安装外部工具（也无需 git）。包源为 **GitHub 仓库页 URL / HTTP(S) zip 直链 / 本地路径**（目录或 zip）。

## 命令

```bash
tphp new <name>              # 创建应用项目（main.php + mod.json）
tphp new --lib <name>        # 创建库包（src/ + tests/ + autoload）

tphp add <source> [ref]      # 添加依赖；source 为 GitHub 仓库 URL / zip URL / 路径
tphp remove <name>           # 移除依赖（如 foo/bar）
tphp install                 # 按 mod.lock 恢复缺失依赖（CI/新机器）
tphp upload                  # 打印发布指引（占位，见下）
```

所有命令在**项目根目录**（含 `mod.json` 的目录）执行。

## 清单 `mod.json`

```json
{
  "name": "my-app",
  "version": "0.1.0",
  "type": "app",
  "author": "You <you@example.com>",
  "description": "My application",
  "license": "MIT",
  "autoload": {
    "psr-4": { "App\\": "src/" },
    "files": ["src/helpers.php"]
  },
  "deps": {
    "foo/bar": {
      "url": "https://example.com/foo-bar.zip",
      "ref": "v1.2.0"
    }
  }
}
```

| 字段 | 说明 |
|---|---|
| `name` | 包 ID。应用=单段（`my-app`）；库=`foo/bar`（映射 `vendor/foo/bar/`） |
| `version` | 语义化版本 |
| `type` | `app` \| `lib` |
| `autoload.psr-4` | 命名空间前缀 → 目录，可多条 |
| `autoload.files` | 加载 autoload 时无条件 require 的文件（函数库等） |
| `deps` | 依赖：`url` 可为 GitHub 仓库 URL（支持 `:ref` 后缀）、http(s) zip URL 或相对/绝对路径；`ref` = 显式指定或解析固化的版本 |

**无 `main` 字段**：应用入口由命令行指定（`tphp run main.php`）。

## 目录布局

```
my-project/
├── mod.json
├── mod.lock               # 生成物，建议提交（可复现 install）
├── main.php               # 应用入口
└── vendor/                # 依赖（.gitignore 忽略）
    ├── autoload.php       # 生成物：根项目 + 依赖的 psr-4/files 合并
    ├── foo/bar/
    └── acme/utils/
```

- 包 ID `foo/bar` → `vendor/foo/bar/`
- 全项目共享一个扁平 vendor，传递依赖不嵌套
- 包名以包内 `mod.json` 的 `name` 为准

## 使用流程

### 创建项目

```bash
tphp new demo-app
cd demo-app
```

`main.php` 已含 `require "vendor/autoload.php";`。

### 添加依赖

```bash
# GitHub 仓库链接
tphp add https://github.com/tphp-lang/lib_rt              # 解析最新发布版并固化 ref
tphp add https://github.com/tphp-lang/lib_rt:master       # URL 后缀指定分支
tphp add https://github.com/tphp-lang/lib_rt:v0.0.1       # URL 后缀指定 tag
tphp add https://github.com/tphp-lang/lib_rt v0.0.1       # CLI 参数指定 ref

# HTTP zip（URL 即版本锚点）
tphp add https://example.com/releases/foo-bar-1.2.0.zip

# 本地目录（开发中互指）
tphp add ../my-lib

# 本地 zip
tphp add ./foo-bar.zip
```

GitHub 仓库链接规则（无 git、无外网工具依赖；解析最新发布版用 GitHub 匿名 API，限流 60 次/小时/IP）：

- **无 ref**：查 `releases/latest`（失败回退 `tags` 本地 semver 排序，再失败回退默认分支）→ 下载该版本 zip → **把解析出的 ref 固化进 `mod.json`**（可复现）
- **`:ref` 后缀或 CLI ref 参数**：直接下 `zipball/<ref>`（tag/分支皆可；冲突时 CLI 参数优先；含 `/` 的 ref 只能用 CLI 参数形式）
- mod.json 存**剥掉 `:ref` 后缀的仓库 URL** + `ref` 字段；再次 `add` 同名依赖 = 重新解析并刷新 ref（无 ref 的裸链接 = 跟最新发布升级）
- 支持 `owner/repo`、可选 `.git` 尾缀、`/tree/<分支>` 路径（优先级低于 `:ref`）

`add` 会：装入 `vendor/<name>/` → 递归安装传递依赖 → 更新 `mod.json` deps → 重写 `mod.lock` → 重新生成 `vendor/autoload.php`。

同名依赖已存在时 = 更新（覆盖 vendor 目录）。**覆盖前有安全检查**（fail-closed，对齐 vpm）：目标目录无 `mod.json`、含 `.git` 工作区、或 `mod.json` 包名不符时拒绝覆盖并报错——防止误删你手动放进 vendor 的工作区；确认无用后手动删除该目录即可继续。

### 使用依赖

```php
<?php
require "vendor/autoload.php";

use Foo\Bar\Str;   // 自动加载 vendor/foo/bar/src/Str.php

echo Str::upper("hi");
```

无 autoload 的包可显式路径 require：

```php
require "vendor/foo/bar/src/util.php";
```

### 恢复依赖（CI / 新机器）

```bash
git clone ... && cd project
tphp install        # 按 mod.lock 恢复；已装跳过（离线友好）
tphp run main.php
```

无 `mod.lock` 时从 `mod.json` deps 全量递归解析。

### 移除依赖

```bash
tphp remove foo/bar
```

从 deps 删除、清理 `vendor/foo/bar/`，并删除不再被任何包引用的传递依赖孤儿。

### 发布一个库包

```bash
cd my-lib
# 编辑 mod.json 的 version
git add mod.json && git commit -m "release 1.2.0"
git tag v1.2.0 && git push --tags
# 将仓库打 zip 上传到任意可直链的位置（Releases/对象存储均可）
```

`tphp upload` 打印上述指引。**发布 = git tag + 可下载 zip**，无中央注册表（未来若做，`add <pkg>` 无 URL 时才走注册表，lock 结构不变）。

## 与 `tphp build` 的衔接

```bash
tphp build main.php -o app
```

- 工作目录有 `mod.json` 且存在 `vendor/autoload.php`、且未显式 `--with` → **自动打包 `vendor/`**
- 自动 vendor 时排除：`.git` 等 VCS/系统垃圾、`tests/`、`*.md`
- 显式 `--with <dir>` 不启用上述扩展排除（仅跳过 VCS/系统垃圾）
- vendor 内 `.php` 源码进 zip（autoload 运行期 `OP_REQUIRE` 依赖它）

手动指定打包集：

```bash
tphp build main.php --with assets/ --with vendor/ -o app
```

## 边界与限制

| 项 | 说明 |
|---|---|
| 私有仓库 | 不自建认证、不依赖 git；HTTPS 信任链 = 内嵌 Mozilla CA ∪ 系统 Root 存储（Windows 系统证书库） |
| 版本解析 | GitHub 裸仓库 URL 无 ref 时调匿名 API 解析最新发布（限流 60 次/小时/IP）并固化 ref；zip 直链以 URL 为版本锚点，`ref` 仅记录 |
| 嵌套 vendor | 禁止（全项目共享扁平 vendor） |
| `pack` 字段 | `pack.include`/`pack.exclude` 已在 mod.json 解析与保留，`tphp build` 消费后置 |
| autoload.classmap | 未支持（PSR-4 + files 覆盖主流） |

## 相关文档

- [PLAN/pkg-manager.md](../PLAN/pkg-manager.md) — 设计规格与实施阶段
