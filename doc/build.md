# 构建

纯命令行脚本，无 Make/CMake。所有脚本为 **ASCII-only**。

源码扫描：`src/*.c`（引擎）+ `ext/*/*.c`（内置扩展，一扩展一子目录）；扩展头文件经 `-Iext/<name>` 引用（排在 libffi include 之后）。

## Windows（msys2 clang64）

前置：安装 [MSYS2](https://www.msys2.org/)，`CLANG64` 环境含 `clang`。

```bat
rem 仓库根目录
build.cmd
```

- 产物：`build\tphp.exe`
- 两阶段：编译链接 → 若 `lib\*.php` 存在则 `tphp phpc lib` 重打 `bundle_data.c` 再链
- 链接 vendored `deps\libffi\lib\libffi.a`（mingw 静态库）

## Linux

```bash
# 依赖：clang、libffi-dev、binutils（objcopy）
sudo apt-get install -y clang libffi-dev binutils

./build.sh
```

- 产物：`build/tphp`
- libffi 走系统（`pkg-config` 或 `-lffi`），**不**链 vendored `.a`（那是 Windows COFF）
- libuv 按平台选源文件（linux.c 提供 platform loop / hrtime；不编 posix fallback）

## macOS

```bash
brew install libffi
./build.sh                 # native（arm64 或 x86_64）
TARGET_ARCH=x86_64 ./build.sh   # arm64 机上交叉编译 x86_64
```

- 交叉编译时 `-arch $TARGET_ARCH` 同时加到**编译和链接**
- pkg-config 的 libffi 是宿主架构的，交叉时跳过、改用 `-lffi`（SDK）

## 清理

```bash
rm -rf build/obj build/tphp        # Unix
rd /s /q build\obj                 # Windows
```

## CI

`.github/workflows/ci.yml`，5 个 job：

| Job | Runner | 构建 |
|---|---|---|
| windows-x86_64 | windows-latest + msys2 CLANG64 | build.cmd |
| linux-x86_64 | ubuntu-latest | build.sh |
| linux-aarch64 | ubuntu-24.04-arm | build.sh |
| macos-aarch64 | macos-latest | build.sh |
| macos-x86_64 | macos-latest 交叉 | `TARGET_ARCH=x86_64 build.sh` |

每个 job：**build → tests/run.ps1 → pack smoke → tphp -v + tphp test tests/lib → 上传 artifact**。

## ASAN 调试构建（手工，未入库）

```bat
rem src 重编 -O1 -fsanitize=address（不用 mimalloc）
rem 链接 build/obj 下非 src 的依赖 .o + libffi
rem fiber.c 有 __sanitizer_start/finish_switch_fiber 注解
```
