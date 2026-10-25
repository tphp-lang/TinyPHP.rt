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
ARCH_FLAG=""
if [ "$TARCH" != "$HOST_ARCH" ]; then
  case "$TOS" in
    Darwin)
      ARCH_FLAG="-arch $TARCH"
      CFLAGS="$CFLAGS $ARCH_FLAG"
      CFLAGS_US="$CFLAGS_US $ARCH_FLAG"
      CROSS=1
      ;;
    *)
      echo "build.sh: cross-compile on $TOS not supported (only Darwin -arch)" >&2
      exit 1
      ;;
  esac
  echo "tphp build: cross-compile $HOST_ARCH -> $TARCH"
fi
INC="-Isrc -Ideps/libuv/include -Ideps/libuv/src -Ideps/mimalloc/include -Ideps/mimalloc/src -Ideps/mbedtls/include -Ideps/mbedtls/library -Ideps/miniz -Ideps/pcre2"

# libffi: vendored headers OK (arch-independent); libs must be host-arch
# (never deps/libffi/lib/libffi.a: mingw COFF, see link step)
FFI_CFLAGS=""
FFI_LIBS=""
if [ -d deps/libffi/include ]; then
  INC="$INC -Ideps/libffi/include"
fi
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libffi; then
  if [ -z "$FFI_CFLAGS" ]; then
    FFI_CFLAGS=$(pkg-config --cflags libffi)
    INC="$INC $FFI_CFLAGS"
  fi
  if [ "$CROSS" = 0 ]; then
    FFI_LIBS=$(pkg-config --libs libffi)
  fi
fi
if [ -z "$FFI_LIBS" ]; then
  FFI_LIBS="-lffi"
fi

# ext\* include dirs MUST come after any libffi include: libffi's public
# header is <ffi.h>; an -Iext/ffi earlier in the list with an ffi.h inside
# would shadow it (same trap as the historic src/ffi.h naming ban).
INC="$INC -Iext/ffi -Iext/net -Iext/io -Iext/worker -Iext/json -Iext/serialize -Iext/pkg -Iext/builtin -Iext/standard -Iext/pcre"

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
# libuv >=1.45: common sources live in src/ root (threadpool/timer/idna/inet/...)
UV_UNIX_BASE="async.c core.c dl.c fs.c getaddrinfo.c getnameinfo.c loop-watcher.c loop.c pipe.c poll.c process.c signal.c stream.c tcp.c thread.c tty.c udp.c"
# NOTE: posix-poll.c / posix-hrtime.c are fallbacks for platforms WITHOUT a
# native backend (uv/posix.h poll_fds fields). Linux uses linux.c (epoll),
# Darwin uses darwin.c (kqueue); compiling the fallbacks would duplicate
# uv__platform_loop_init/uv__hrtime and reference missing poll_fds fields.
UV_COMMON="fs-poll.c idna.c inet.c random.c strscpy.c strtok.c thread-common.c threadpool.c timer.c uv-common.c uv-data-getter-setters.c version.c"
# libuv on Linux needs GNU extensions (cpu_set_t etc). gnu11 + _GNU_SOURCE
# before any system header; -include guard covers feature-macro races.
UV_CFLAGS="-O2 -g -fno-omit-frame-pointer -std=gnu11 $ARCH_FLAG"
echo '#ifndef _GNU_SOURCE' > "$OBJ/uv_force.h"
echo '#define _GNU_SOURCE 1' >> "$OBJ/uv_force.h"
echo '#endif' >> "$OBJ/uv_force.h"

case "$TOS" in
  Linux)
    # linux.c already provides uv_loadavg / uv_get_*_memory / uv__hrtime /
    # uv__platform_loop_init. Do NOT add sysinfo-loadavg/memory.c (fallbacks
    # for other platforms; they would be duplicate symbol definitions).
    UV_UNIX="$UV_UNIX_BASE proctitle.c linux.c procfs-exepath.c random-devurandom.c random-getrandom.c random-sysctl-linux.c"
    UV_DEFS="-D_GNU_SOURCE -D_DEFAULT_SOURCE"
    LDLIBS="-lpthread -ldl -lrt -lm"
    ;;
  Darwin)
    # random.c's __APPLE__ branch calls uv__random_devurandom as fallback
    # (getentropy failure path); must compile random-devurandom.c too.
    UV_UNIX="$UV_UNIX_BASE proctitle.c bsd-ifaddrs.c kqueue.c random-getentropy.c random-devurandom.c darwin-proctitle.c darwin.c fsevents.c"
    UV_DEFS="-D_DARWIN_UNLIMITED_SELECT=1 -D_DARWIN_USE_64_BIT_INODE=1"
    LDLIBS="-lm -framework CoreFoundation -framework CoreServices"
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
for f in $UV_COMMON; do
  $CC -w $UV_CFLAGS $UV_DEFS -include "$OBJ/uv_force.h" $INC \
      -c "deps/libuv/src/$f" -o "$OBJ/uv_$(basename "$f" .c).o"
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
# _DEFAULT_SOURCE: glibc exposes ftello/fseeko (MZ_FTELL64/MZ_FSEEK64) only
# under POSIX feature macros; macOS libc declares them by default
$CC -w $CFLAGS -D_DEFAULT_SOURCE $INC -c deps/miniz/miniz.c -o "$OBJ/miniz.o"

# ---- pcre2 (vendored php-src pcre2lib; no JIT; PHP source whitelist) ----
echo "[4b/6] compiling pcre2 (no JIT)..."
PCRE2_DEFS="-DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 -DPCRE2_STATIC=1"
PCRE2_SRCS="pcre2_auto_possess pcre2_chartables pcre2_compile pcre2_config pcre2_context pcre2_chkdint pcre2_dfa_match pcre2_error pcre2_jit_compile pcre2_maketables pcre2_match pcre2_match_data pcre2_newline pcre2_ord2utf pcre2_pattern_info pcre2_serialize pcre2_string_utils pcre2_study pcre2_substitute pcre2_substring pcre2_tables pcre2_ucd pcre2_valid_utf pcre2_xclass pcre2_find_bracket pcre2_convert pcre2_extuni pcre2_script_run"
for f in $PCRE2_SRCS; do
  $CC -w $CFLAGS $PCRE2_DEFS -Ideps/pcre2 -c "deps/pcre2/$f.c" -o "$OBJ/pcre2_$f.o"
done

# ---- IR JIT backend (PLAN/ir-jit.md; x86_64 only) ----
# aarch64: jit.c compiles as the disabled stub (no IR objects, no TPHP_JIT).
JIT_DEFS=""
if [ "$TARCH" = "x86_64" ]; then
  echo "[4c/6] compiling IR (JIT backend, x86_64)..."
  INC="$INC -Ideps/ir"
  JIT_DEFS="-DTPHP_JIT"
  IR_SRCS="ir ir_cfg ir_check ir_dump ir_emit ir_emit_c ir_gcm ir_gdb ir_load ir_mem2ssa ir_patch ir_ra ir_ra_simple ir_save ir_sccp ir_strtab"
  for f in $IR_SRCS; do
    $CC -w $CFLAGS -DIR_TARGET_X64 -Ideps/ir -c "deps/ir/$f.c" -o "$OBJ/ir_$f.o"
  done
else
  echo "[4c/6] JIT backend skipped (aarch64 v1: disabled stub)"
fi

# ---- [5/6] tphp sources (src + ext) ----
echo "[5/6] compiling tphp sources..."
DEFS="$DEFS $JIT_DEFS"
for f in src/*.c; do
  $CC $CFLAGS_US $DEFS $INC -c "$f" -o "$OBJ/src_$(basename "$f" .c).o"
done
for d in ext/*/; do
  for f in "$d"*.c; do
    [ -e "$f" ] || continue
    $CC $CFLAGS_US $DEFS $INC -c "$f" -o "$OBJ/src_$(basename "$f" .c).o"
  done
done

# ---- [6/6] link ----
echo "[6/6] linking..."
# Unix NEVER links deps/libffi/lib/libffi.a (mingw COFF; Darwin ld rejects it).
# Always system libffi: pkg-config or -lffi.
if [ -n "$FFI_LIBS" ]; then
  FFI_LIB="$FFI_LIBS"
else
  FFI_LIB="-lffi"
fi
echo "  link ffi: $FFI_LIB arch: ${ARCH_FLAG:-default}"
# ARCH_FLAG must be on the link too when cross-compiling (else ld defaults
# to host arch and ignores the -arch-built objects: "Undefined: _main").
$CC $ARCH_FLAG "$OBJ"/*.o $FFI_LIB -o "$OUT" $LDLIBS

echo "Build OK: $OUT"
