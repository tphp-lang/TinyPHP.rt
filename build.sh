#!/bin/sh
# TinyPHP.rt build script (Linux / macOS, clang or gcc). ASCII only.
# Usage: ./build.sh
set -e

# ---- toolchain ----
if command -v clang >/dev/null 2>&1; then
  CC=clang
else
  CC=cc
fi

OUT=build/tphp
OBJ=build/obj
TARCH=$(uname -m)
TOS=$(uname -s)

echo "tphp build: os=$TOS arch=$TARCH cc=$CC"

# ---- flags ----
CFLAGS="-O2 -std=c11 -g -fno-omit-frame-pointer"
CFLAGS_US="-O2 -std=c11 -Wall -Wextra -g -fno-omit-frame-pointer -D_GNU_SOURCE"
INC="-Isrc -Ideps/libuv/include -Ideps/libuv/src -Ideps/mimalloc/include -Ideps/mimalloc/src"
DEFS="-DTPHP_TARGET=\"${TARCH}-${TOS,,}\""

mkdir -p build "$OBJ"

# ---- [1/4] vendored deps ----
echo "[1/4] compiling vendored deps (libuv, mimalloc)..."
UV_UNIX_BASE="async.c core.c dl.c fs.c getaddrinfo.c getnameinfo.c loop-watcher.c loop.c pipe.c poll.c process.c random-devurandom.c signal.c stream.c tcp.c thread.c tty.c udp.c"

case "$TOS" in
  Linux)
    UV_UNIX="$UV_UNIX_BASE proctitle.c linux.c procfs-exepath.c random-getrandom.c random-sysctl-linux.c"
    UV_DEFS="-D_GNU_SOURCE -D_POSIX_C_SOURCE=200112"
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
  $CC -w $CFLAGS $UV_DEFS $INC -c "deps/libuv/src/unix/$f" -o "$OBJ/uv_$(basename "$f" .c).o"
done

# mimalloc: explicit list from its CMakeLists (alloc-override.c / free.c are internal includes)
MI_SRCS="alloc.c alloc-aligned.c alloc-posix.c arena.c bitmap.c heap.c init.c libc.c options.c os.c page.c page-map.c random.c sample-guarded.c sample-profile.c stats.c subproc.c theap.c threadlocal.c"
for f in $MI_SRCS; do
  $CC -w $CFLAGS -DMI_STATIC_LIB $INC -c "deps/mimalloc/src/$f" -o "$OBJ/mi_$(basename "$f" .c).o"
done
for f in deps/mimalloc/src/prim/*.c; do
  $CC -w $CFLAGS -DMI_STATIC_LIB $INC -c "$f" -o "$OBJ/mip_$(basename "$f" .c).o"
done

# ---- [2/4] fiber asm ----
echo "[2/4] compiling fiber asm ($TARCH)..."
case "$TOS" in
  Linux)
    ASM_ARCH="*_x86_64_sysv_elf_gas.S"  # x86_64 only for now
    ;;
  Darwin)
    ASM_ARCH="*_x86_64_sysv_macho_gas.S"  # Intel mac only; aarch64 asm lands with M4
    ;;
esac
for f in deps/asm/$ASM_ARCH; do
  $CC -w $CFLAGS -c "$f" -o "$OBJ/asm_$(basename "$f" .S).o"
done

# ---- [3/4] tphp sources ----
echo "[3/4] compiling tphp sources..."
for f in src/*.c; do
  $CC $CFLAGS_US $DEFS $INC -c "$f" -o "$OBJ/src_$(basename "$f" .c).o"
done

# ---- [4/4] link ----
echo "[4/4] linking..."
$CC "$OBJ"/*.o -o "$OUT" $LDLIBS

echo "Build OK: $OUT"
