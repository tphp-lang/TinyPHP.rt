# 构建

纯命令行脚本，无 Make/CMake。所有脚本为 **ASCII-only**。

源码扫描：`src/*.c`（引擎）+ `ext/*/*.c`（内置扩展，一扩展一子目录）；扩展头文件经 `-Iext/<name>` 引用。

## Windows（msys2 clang64）

前置：安装 [MSYS2](https://www.msys2.org/)，`CLANG64` 环境含 `clang`。

```bat
rem 仓库根目录
build.cmd
```

- 产物：`build\tphp.exe`
- 两阶段：编译链接 → 若 `lib\*.php` 存在则 `tphp phpc lib` 重打 `bundle_data.c` 再链

## Linux

```bash
# 依赖：clang、binutils（objcopy）
sudo apt-get install -y clang binutils

./build.sh
```

- 产物：`build/tphp`
- libuv 按平台选源文件（linux.c 提供 platform loop / hrtime；不编 posix fallback）

## macOS

```bash
./build.sh                 # native（arm64 或 x86_64）
TARGET_ARCH=x86_64 ./build.sh   # arm64 机上交叉编译 x86_64
```

- 交叉编译时 `-arch $TARGET_ARCH` 同时加到**编译和链接**

## 清理

```bash
rm -rf build/obj build/tphp        # Unix
rd /s /q build\obj                 # Windows
```

## CI

**拆分（11-05）**：测试自动、打包手动，只保留三个工作流——

- `.github/workflows/test.yml`：**自动**（push/PR）——build → tests/run.ps1 → pack smoke → smoke，**不导出产物**
- `.github/workflows/build.yml`：**仅手动**（workflow_dispatch）——五平台 build → Sanity(`-v`) → 导出 artifact（binary + libtphp.a + include/），不重跑测试
- `.github/workflows/perf.yml`：**仅手动**——13 项 perf 套 + mimalloc 变体对照

| Job | Runner | 构建 |
|---|---|---|
| windows-x86_64 | windows-latest + msys2 CLANG64 | build.cmd |
| linux-x86_64 | ubuntu-latest | build.sh |
| linux-aarch64 | ubuntu-24.04-arm | build.sh |
| macos-aarch64 | macos-latest | build.sh |
| macos-x86_64 | macos-latest 交叉 | `TARGET_ARCH=x86_64 build.sh` |

test.yml 每个 job：**build → tests/run.ps1 → pack smoke → smoke**；build.yml 每个 job：**build → Sanity → 上传 artifact**。

## ASAN 调试构建（手工，未入库）

```bat
rem src 重编 -O1 -fsanitize=address（不用 mimalloc）
rem 链接 build/obj 下非 src 的依赖 .o
rem fiber.c 有 __sanitizer_start/finish_switch_fiber 注解
```
