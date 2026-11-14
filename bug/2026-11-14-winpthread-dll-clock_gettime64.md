# bug：tphp.exe 启动期动态依赖 clang64 的 libwinpthread-1.dll（缺 PATH 即 0xC0000135）

> 状态：**已修**（2026-11-14，主窗口会话；commit 41daece）
> 严重度：中——exe 不可自包含/分发；任何未挂 clang64 PATH 的进程环境
> （如桌面 App 子进程、双击运行）一律启动失败且**零输出**。

## 修复记录（2026-11-14 主窗口，采纳方案①并扩充）

- **方案①两处落地 + 一个报告未覆盖的面**：
  1. [build.cmd](../build.cmd) 两条链接行（`:59`/`:68`）末尾追加 `-static`；
  2. **[ext/pkg/pkg.c](../ext/pkg/pkg.c) `ext_engine_libs` WIN 行追加
     `-static`**——`ext_link_and_replace` 会用引擎库行重链 **tphp.exe
     本体**（tphp.next → 原子替换），不加则任何 `ext --add`/ext relink
     都会把静态 exe **打回动态 winpthread 依赖**。实证：全量套件中
     test-pkg-ext 触发 relink（`build/ext/.registry.o` 15:49:58.233 →
     exe 原子替换 15:49:58.361），修复后 relink 产物仍自包含；修复前
     本会话同样观测到「build 完静态 exe → 套件跑完变动态」的完整过程。
- 验证：`objdump -p build/tphp.exe` 导入表非系统 DLL 仅剩 0 个
  winpthread（api-ms-win-crt-* 均为系统 UCRT）；纯系统 PATH
  （`/c/windows/system32:/c/windows`）直启 `run` 脚本 exit=0；
  全量套件 250/250 通过且**套件后**复验导入表仍为 0。
- 报告取证链全数核实：单符号 `clock_gettime64` 导入 ✓、Git mingw64
  DLL 0 导出 ✓、clang64 DLL 有导出 ✓、链接行无 -static ✓、
  test.yml:41 CI 注入 ✓。两死法均真实：无兼容 DLL = 0xC0000135，
  Git 旧版 DLL 挡 PATH = 0xC0000139（入口点找不到）。

## 症状

`build/tphp.exe` 在 PATH 不含 `C:\env\msys2\clang64\bin` 的环境启动即失败：

- Git Bash：`exit 127`（极易误判为"命令不存在"）
- PowerShell：`-1073741511` = `0xC0000135` STATUS_DLL_NOT_FOUND
- stdout/stderr 全空，无任何诊断线索

临时解法：`PATH=/c/env/msys2/clang64/bin:$PATH` 前缀。CI 不受影响
（[.github/workflows/test.yml:41](../.github/workflows/test.yml) 已注入
`/clang64/bin` 到 GITHUB_PATH）。

## 取证链（已定位到符号级）

1. `objdump -p build/tphp.exe` 导入表：非系统 DLL **只有**
   `libwinpthread-1.dll`，且**仅导入一个符号 `clock_gettime64`**——
   winpthreads 头的 time64 重定向（civetweb 对 clock_gettime 的用法触发；
   teb 包 mod.json 注释「Windows 的 -lwinpthread 来自 civetweb 的 time64
   重定向」与此互证）。
2. `C:\env\msys2\clang64\bin\libwinpthread-1.dll` 导出 `clock_gettime64` ✓
3. Git 自带的 `C:\Program Files\Git\mingw64\bin\libwinpthread-1.dll`
   **不导出**该符号（旧版 winpthread，实测 `objdump | grep -c` = 0）——
   所以「把 Git mingw64 加进 PATH」救不了，实测无效（exit 127 复现）。
4. 根因：[build.cmd:59](../build.cmd) / [build.cmd:68](../build.cmd) 链接行
   无 `-static` → winpthread 动态挂靠（对比：引擎已有
   [build.cmd:51-52](../build.cmd) `chkstk_stub.S` 处理过同性质的
   运行期符号缺口）。

## 修复方向（供分析取舍）

| 方案 | 做法 | 评价 |
|---|---|---|
| ① 链接行加 `-static` | build.cmd 两处链接命令末尾追加 `-static`（静态挂 winpthread + libgcc；clang64 自带静态库，CI 同环境） | **推荐**：一行改动，exe 自包含；代价是二进制略大 |
| ② 构建后拷 DLL | build.cmd 末尾把 clang64 的 libwinpthread-1.dll 拷到 exe 旁 | 打包机必须装有 clang64；artifact 自包含性依赖拷贝步骤 |
| ③ clock_gettime64 桩 | 仿 chkstk_stub.S 提供 `clock_gettime64` 实现，彻底移除 winpthread 导入 | 最彻底但需手对 time64 ABI（`struct __timespec64` 布局/时钟语义），风险最高 |

## 附注（澄清两件相关但不相同的事）

- **tcc 与本题无关**：tcc 是 ext 编译的编译期回落工具（cc 探测序
  [ext/pkg/pkg.c:3984](../ext/pkg/pkg.c)：os.cc → `TPHP_EXT_CC` →
  `<eroot>/tcc/tcc` → PATH clang/gcc），解决不了已编译 exe 的运行期
  DLL 依赖。
- **eroot = exe 所在目录**（[ext/pkg/pkg.c:1445](../ext/pkg/pkg.c)
  `tphp_pkg_ext_root` = g_self_exe 去文件名）→ repo 根的 `tcc/` 对
  `build/tphp.exe` 的 ext 编译**不生效**（只探测 `build/tcc/`）；
  本机 `ext --add` 实际落到 PATH clang。若想让本地 ext 编译不依赖
  clang，把 tccbin 解包到 `build/tcc/` 即可（运行期 DLL 问题仍需上述
  方案①/②/③）。

## 关联

- 本目录 2026-11-14-strxor-both-strings-returns-int.md（同会话发现的另一项）
