#!/bin/sh
# TinyPHP.rt build script (Linux / macOS, clang or gcc). ASCII only.
# Usage: ./build.sh
# Supports: linux x86_64/aarch64, darwin x86_64/arm64.
# Cross-compile: TARGET_ARCH=x86_64 ./build.sh   (e.g. arm64 mac runner -> x86_64)
set -e

# ---- toolchain ----
if command -v clang >/dev/null 2>&1; then
  CC=clang
else
  CC=cc
fi

OUT=build/tphp
OBJ=build/obj
TARCH=${TARGET_ARCH:-$(uname -m)}
TOS=$(uname -s)
# portable lowercase (sh/dash has no ${var,,})
TOS_LC=$(printf '%s' "$TOS" | tr 'A-Z' 'a-z')

echo "tphp build: os=$TOS arch=$TARCH cc=$CC"

# ---- flags ----
CFLAGS="-O2 -std=c11 -g -fno-omit-frame-pointer"
CFLAGS_US="-O2 -std=c11 -Wall -Wextra -g -fno-omit-frame-pointer -D_GNU_SOURCE -DTPHP_USE_MIMALLOC"
# cross-compile: add -arch to clang (macOS) when TARGET_ARCH differs from host
HOST_ARCH=$(uname -m)
CROSS=0
if [ "$TARCH" != "$HOST_ARCH" ]; then
  case "$TOS" in
    Darwin)
      CFLAGS="$CFLAGS -arch $TARCH"
      CFLAGS_US="$CFLAGS_US -arch $TARCH"
      CROSS=1
      ;;
    *)
      echo "build.sh: cross-compile on $TOS not supported (only Darwin -arch)" >&2
      exit 1
      ;;
  esac
  echo "tphp build: cross-compile $HOST_ARCH -> $TARCH"
fi
INC="-Isrc -Ideps/libuv/include -Ideps/libuv/src -Ideps/mimalloc/include -Ideps/mimalloc/src -Ideps/mbedtls/include -Ideps/mbedtls/library -Ideps/miniz"

# libffi: vendored headers if present, else pkg-config, else system -lffi
# cross-compile: skip pkg-config libs (brew is host-arch only); use SDK -lffi
FFI_CFLAGS=""
FFI_LIBS=""
if [ -d deps/libffi/include ]; then
  INC="$INC -Ideps/libffi/include"
elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libffi; then
  FFI_CFLAGS=$(pkg-config --cflags libffi)
  if [ "$CROSS" = 0 ]; then
    FFI_LIBS=$(pkg-config --libs libffi)
  fi
  INC="$INC $FFI_CFLAGS"
fi
if [ "$CROSS" = 1 ] && [ -z "$FFI_LIBS" ]; then
  FFI_LIBS="-lffi"
fi

# normalize arch: arm64 -> aarch64 for target string (Linux); keep arm64 on Darwin
case "$TARCH" in
  arm64) TARCH_N=arm64 ;;
  aarch64) TARCH_N=aarch64 ;;
  *) TARCH_N=$TARCH ;;
esac
DEFS="-DTPHP_TARGET=\"${TARCH_N}-${TOS_LC}\""

mkdir -p build "$OBJ"

# ---- [1/6] libuv ----
echo "[1/6] compiling libuv..."
UV_UNIX_BASE="async.c core.c dl.c fs.c getaddrinfo.c getnameinfo.c loop-watcher.c loop.c pipe.c poll.c process.c random-devurandom.c signal.c stream.c tcp.c thread.c tty.c udp.c"
# libuv on Linux needs GNU extensions (cpu_set_t etc). gnu11 + _GNU_SOURCE
# before any system header; -include guard covers feature-macro races.
UV_CFLAGS="-O2 -g -fno-omit-frame-pointer -std=gnu11"
echo '#ifndef _GNU_SOURCE' > "$OBJ/uv_force.h"
echo '#define _GNU_SOURCE 1' >> "$OBJ/uv_force.h"
echo '#endif' >> "$OBJ/uv_force.h"

case "$TOS" in
  Linux)
    UV_UNIX="$UV_UNIX_BASE proctitle.c linux.c procfs-exepath.c random-getrandom.c random-sysctl-linux.c"
    UV_DEFS="-D_GNU_SOURCE -D_DEFAULT_SOURCE"
    LDLIBS="-lpthread -ldl -lrt -lm"
    ;;
  Darwin)
    UV_UNIX="$UV_UNIX_BASE proctitle.c bsd-ifaddrs.c kqueue.c random-getentropy.c darwin-proctitle.c darwin.c fsevents.c"
    UV_DEFS="-D_DARWIN_UNLIMITED_SELECT=1 -D_DARWIN_USE_64_BIT_INODE=1"
    LDLIBS="-lm"
    ;;
  *)
    echo "build.sh: unsupported OS: $TOS" >&2
    exit 1
    ;;
esac

for f in $UV_UNIX; do
  # echo first file's command for CI diagnostics
  if [ "$f" = "async.c" ]; then
    echo "  UV cmd: $CC -w $UV_CFLAGS $UV_DEFS -include $OBJ/uv_force.h $INC -c deps/libuv/src/unix/$f"
  fi
  $CC -w $UV_CFLAGS $UV_DEFS -include "$OBJ/uv_force.h" $INC \
      -c "deps/libuv/src/unix/$f" -o "$OBJ/uv_$(basename "$f" .c).o"
done

# ---- [2/6] mimalloc ----
echo "[2/6] compiling mimalloc..."
MI_SRCS="alloc.c alloc-aligned.c alloc-posix.c arena.c bitmap.c heap.c init.c libc.c options.c os.c page.c page-map.c random.c sample-guarded.c sample-profile.c stats.c subproc.c theap.c threadlocal.c"
for f in $MI_SRCS; do
  $CC -w $CFLAGS -DMI_STATIC_LIB $INC -c "deps/mimalloc/src/$f" -o "$OBJ/mi_$(basename "$f" .c).o"
done
for f in deps/mimalloc/src/prim/*.c; do
  $CC -w $CFLAGS -DMI_STATIC_LIB $INC -c "$f" -o "$OBJ/mip_$(basename "$f" .c).o"
done

# ---- [3/6] fiber asm (per OS + arch) ----
echo "[3/6] compiling fiber asm ($TOS $TARCH)..."
case "$TOS" in
  Linux)
    case "$TARCH" in
      x86_64)  ASM_GLOB="*_x86_64_sysv_elf_gas.S" ;;
      aarch64) ASM_GLOB="*_arm64_aapcs_elf_gas.S" ;;
      *) echo "build.sh: unsupported Linux arch: $TARCH" >&2; exit 1 ;;
    esac
    ;;
  Darwin)
    case "$TARCH" in
      x86_64) ASM_GLOB="*_x86_64_sysv_macho_gas.S" ;;
      arm64)  ASM_GLOB="*_arm64_aapcs_macho_gas.S" ;;
      *) echo "build.sh: unsupported Darwin arch: $TARCH" >&2; exit 1 ;;
    esac
    ;;
esac
for f in deps/asm/$ASM_GLOB; do
  $CC -w $CFLAGS -c "$f" -o "$OBJ/asm_$(basename "$f" .S).o"
done

# ---- [4/6] mbedtls + miniz ----
echo "[4/6] compiling mbedtls + miniz..."
for f in deps/mbedtls/library/*.c; do
  $CC -w $CFLAGS $INC -c "$f" -o "$OBJ/mb_$(basename "$f" .c).o"
done
$CC -w $CFLAGS $INC -c deps/miniz/miniz.c -o "$OBJ/miniz.o"

# ---- [5/6] tphp sources ----
echo "[5/6] compiling tphp sources..."
for f in src/*.c; do
  $CC $CFLAGS_US $DEFS $INC -c "$f" -o "$OBJ/src_$(basename "$f" .c).o"
done

# ---- [6/6] link ----
echo "[6/6] linking..."
# libffi: vendored static lib if present (Windows layout), else pkg-config/system
if [ -f deps/libffi/lib/libffi.a ]; then
  FFI_LIB="deps/libffi/lib/libffi.a"
elif [ -n "$FFI_LIBS" ]; then
  FFI_LIB="$FFI_LIBS"
else
  FFI_LIB="-lffi"
fi
$CC "$OBJ"/*.o $FFI_LIB -o "$OUT" $LDLIBS

echo "Build OK: $OUT"
