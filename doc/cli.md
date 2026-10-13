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

### 构建单文件应用（跨平台）
```bash
# Windows
tphp build app.php --with vendor\ --with ext\bar.dll -o app.exe
# Linux / macOS
tphp build app.php --with vendor/ --with bar.dll -o app
```

- **`--with`**：文件或目录（可重复）
  - 目录递归收**非 .php** 文件（.php 排除并警告——PHP 走 phpc）
  - 保留相对目录（以参数目录为锚点：`vendor\ssl\foo.dll` → zip 内 `ssl/foo.dll`）
  - 单文件落 zip 根
- 产物含嵌入 zip + 主脚本；**无参数运行**时自动执行主脚本
- 运行期把资产释放到 `<exedir>/runtime/lib`（sha1 校验、原子 rename、zip-slip 防护）

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
