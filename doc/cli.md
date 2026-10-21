# CLI

```
tphp <command> [args...]
```

## 命令

### 运行脚本
```bash
tphp run <file.php> [args...]    # 运行 PHP 脚本
tphp <file.php>                  # 隐式 run（路径不以 - 开头）
tphp -e '<php code>'             # 执行代码串（无 <? 标签时自动补 <?php）
```

### 调试
```bash
tphp --debug ...                 # 在其余命令前加：stderr 打印编译阶段诊断（DIAG:*）
tphp --dump-opcodes <file.php>   # 反汇编编译产物（lex/parse/compile 后 dump，不执行）
tphp -v, --version               # 版本 + 目标三元组 + libuv 版本
tphp -h, --help                  # 帮助
```

### 测试
```bash
tphp test <dir>                  # 扫描 test-*.php，比对 // EXPECT: 与 stdout
```

协议与 `tests/run.ps1` 一致：
- `// EXPECT: <行>` —— 精确匹配 stdout 的一行（**冒号后只跳 1 个空格，保留缩进**）
- `// EXPECT-EXIT: <n>` —— 期望退出码（默认 0）

```bash
tphp test tests/lib               # 只跑某目录
```

### 打包 stdlib（开发用）
```bash
tphp phpc <dir> -o <out.c>        # 把 lib\*.php 源码编进 bundle_data.c
```
`build.cmd` 会自动在链接前调用（若 `lib\` 存在）。

### 包管理（内置，无需 git）
```bash
tphp new <name> [--lib]           # 创建应用 / 库包骨架（mod.json + autoload）
tphp add <source> [ref]           # 添加依赖：GitHub 仓库 URL / zip 直链 / 本地路径
tphp remove <name>                # 移除依赖（如 foo/bar），含孤儿传递依赖清理
tphp install                      # 按 mod.lock 恢复缺失依赖（CI/新机器）
tphp upload                       # 打印发布指引（占位）
```
所有命令在项目根（含 `mod.json` 的目录）执行。详见 [package-manager.md](package-manager.md)。

### 构建单文件应用（跨平台）
```bash
# Windows
tphp build app.php --with vendor\ --with ext\bar.dll -o app.exe
# Linux / macOS
tphp build app.php --with vendor/ --with bar.dll -o app
```

- **`--with`**：文件或目录（可重复）
  - 目录递归收集**全部文件**（含 `.php`——vendor 源码是 autoload 运行期 `OP_REQUIRE` 的目标）；恒跳过 VCS/系统垃圾（`.git`/`.DS_Store` 等）
  - **项目含 `mod.json` + `vendor/autoload.php` 且未显式 `--with vendor/` 时自动打包 `vendor/`**，并额外跳过 `tests/`、`*.md`（strict 模式）；显式 `--with` 不启用 strict（尊重显式指定）
  - **key 相对源码根**（主脚本所在目录）：`--with data` 且主脚本在 `src/` 下
    → zip 内 `data/…`；这让 `__DIR__` 相对读能在打包后自动映射进 zip
  - 绝对路径无法表达包内等效名：单文件退回文件名、目录退回目录内部相对
- 产物含嵌入 zip + 主脚本；**无参数运行**时自动执行主脚本
- **数据资产不落盘**（Bun 式内存直读）：zip 常驻内存。首选地道写法
  `__DIR__` 相对读——未打包读磁盘，打包后磁盘未命中自动回落到 zip（按源码根剥离）：
  ```php
  echo file_get_contents(__DIR__ . "/config.json");
  ```
  `ASSET_ROOT`（值 `tphp://asset`）是直接访问包根的次要途径（不经过源码根）：
  ```php
  echo file_get_contents(ASSET_ROOT . "/config.json");
  ```
  两者都只读；`file_get_contents` / `fopen` / `file_exists` / `filesize` /
  `scandir` 均支持
- **`.dll` / `.so` / `.dylib` 按等效相对路径落盘到 exe 同级**：manifest `type=dll`
  条目提取为 `<exedir>/<包内相对路径>`（如 exe 旁 `vendor/foo.dll`）；磁盘已存在且
  sha1 与清单一致则跳过，否则校验后原子写入。FFI 里直接用包内相对路径即可加载：
  ```php
  $ffi = FFI::cdef("int point_sum(int,int);", "ext/foo.dll");
  ```
- 防护：zip-slip（拒绝 `..`/绝对路径/盘符）；DLL 提取前 sha1 校验

| 平台 | 嵌入方式 |
|---|---|
| Windows | PE RCDATA `TPHP_BUNDLE` + `TPHP_MAIN` |
| Linux | ELF section `.tphp_bundle`（objcopy，失败回落 footer） |
| macOS | 文件尾 footer（`[zip][u64 size][TPHPBND1]`） |

## Windows 编码（中文）

- 启动时设置控制台输出代码页为 UTF-8（`SetConsoleOutputCP`），PHP 源码与 `echo` 输出统一 UTF-8，重定向到文件/管道时输出原始 UTF-8 字节
- 命令行参数按 UTF-8 解析（`-e` 代码串、脚本路径里的中文不依赖系统 ANSI 代码页）
- **PowerShell 5.1 会吃掉 `-e` 参数里成对的双引号**（系统行为，`php` CLI 同样中招）：
  - ✗ `tphp -e 'echo "你好";'` → 实际收到 `echo 你好;` → `Undefined constant "你好"`
  - ✓ `tphp -e "echo '你好';"`（PHP 用单引号）
  - ✓ `tphp -e 'echo \"你好\";'`（转义双引号）
  - pwsh 7.3+ 修正了参数传递，可直接写双引号

## 环境变量

| 变量 | 作用 |
|---|---|
| `TPHP_TRACE=1` | VM 执行轨迹（调试） |
| `TPHP_SER_DEBUG` | serialize 调试输出 |
