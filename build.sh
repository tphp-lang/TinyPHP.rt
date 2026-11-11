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
# NO_MIMALLOC=1 ./build.sh -> CRT allocator build (mem.c #else plain malloc,
# bins shim off), output build/tphp-crt with objects isolated in
# build/obj-crt (shared build/obj would corrupt the main binary). purpose:
# discriminator for the arm64-mac anomaly -- CRT fast => mimalloc Darwin
# behavior is the tax; CRT equally slow => tphp's own layers (GC registry
# TLS / class table) are.
if [ "${NO_MIMALLOC:-0}" = "1" ]; then
  OUT=build/tphp-crt
  OBJ=build/obj-crt
fi
TARCH=${TARGET_ARCH:-$(uname -m)}
TOS=$(uname -s)
# P0-19 cluster-1 fix (11-01): mimalloc's Darwin small-alloc fast path is
# 2.2-3.2x slower than macOS libmalloc (bench_alloc: libc 17-21ns vs mi
# 45-53ns per pair, reproduced across CI rounds; wrapper probe shows the
# gc_list layer is free). Default Darwin to the CRT allocator; opt back in
# with USE_MIMALLOC=1 if a future mimalloc release fixes Darwin.
if [ "$TOS" = "Darwin" ] && [ "${NO_MIMALLOC:-0}" != "1" ] && [ "${USE_MIMALLOC:-0}" != "1" ]; then
  echo "[p0-19] Darwin default: CRT allocator (mimalloc opt-in via USE_MIMALLOC=1)"
  NO_MIMALLOC=1
fi
# P0-19 cluster-2 (11-06 CI): mimalloc aarch64 small-alloc also 3-4x slower
# than libc (bench_alloc: libc 10ns vs mi 38-41ns/pair on Linux aarch64).
# Same Darwin default; opt back in with USE_MIMALLOC=1.
if [ "$TARCH" = "aarch64" ] && [ "${NO_MIMALLOC:-0}" != "1" ] && [ "${USE_MIMALLOC:-0}" != "1" ]; then
  echo "[p0-19] aarch64 default: CRT allocator (mimalloc opt-in via USE_MIMALLOC=1)"
  NO_MIMALLOC=1
fi
# portable lowercase (sh/dash has no ${var,,})
TOS_LC=$(printf '%s' "$TOS" | tr 'A-Z' 'a-z')

echo "tphp build: os=$TOS arch=$TARCH cc=$CC"

# ---- flags ----
CFLAGS="-O2 -std=c11 -g -fno-omit-frame-pointer"
CFLAGS_US="-O2 -std=c11 -Wall -Wextra -g -fno-omit-frame-pointer -D_GNU_SOURCE"
if [ "${NO_MIMALLOC:-0}" != "1" ]; then
  CFLAGS_US="$CFLAGS_US -DTPHP_USE_MIMALLOC"
fi
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

# ext\* include dirs
INC="$INC -Iext/net -Iext/io -Iext/worker -Iext/json -Iext/serialize -Iext/pkg -Iext/builtin -Iext/standard -Iext/pcre"

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
if [ "${NO_MIMALLOC:-0}" != "1" ]; then
echo "[2/6] compiling mimalloc..."
MI_SRCS="alloc.c alloc-aligned.c alloc-posix.c arena.c bitmap.c heap.c init.c libc.c options.c os.c page.c page-map.c random.c sample-guarded.c sample-profile.c stats.c subproc.c theap.c threadlocal.c"
for f in $MI_SRCS; do
  $CC -w $CFLAGS -DMI_STATIC_LIB $INC -c "deps/mimalloc/src/$f" -o "$OBJ/mi_$(basename "$f" .c).o"
done
for f in deps/mimalloc/src/prim/*.c; do
  $CC -w $CFLAGS -DMI_STATIC_LIB $INC -c "$f" -o "$OBJ/mip_$(basename "$f" .c).o"
done
else
echo "[2/6] mimalloc skipped (NO_MIMALLOC=1, CRT allocator build)"
fi

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

# ---- IR JIT backend (PLAN/ir-jit.md; x86_64 + aarch64) ----
# Target flag follows TARCH; emit headers pre-generated in deps/ir/.
JIT_DEFS=""
IR_TARGET=""
case "$TARCH" in
  x86_64) IR_TARGET="X64" ;;
  aarch64|arm64) IR_TARGET="AARCH64" ;;
esac
if [ -n "$IR_TARGET" ]; then
  echo "[4c/6] compiling IR (JIT backend, $IR_TARGET)..."
  INC="$INC -Ideps/ir"
  JIT_DEFS="-DTPHP_JIT"
  IR_SRCS="ir ir_cfg ir_check ir_dump ir_emit ir_emit_c ir_gcm ir_gdb ir_load ir_mem2ssa ir_patch ir_ra ir_ra_simple ir_save ir_sccp ir_strtab"
  for f in $IR_SRCS; do
    $CC -w $CFLAGS -DIR_TARGET_$IR_TARGET -Ideps/ir -c "deps/ir/$f.c" -o "$OBJ/ir_$f.o"
  done
else
  echo "[4c/6] JIT backend skipped (unsupported arch: $TARCH)"
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
# ARCH_FLAG must be on the link too when cross-compiling (else ld defaults
# to host arch and ignores the -arch-built objects: "Undefined: _main").
$CC $ARCH_FLAG "$OBJ"/*.o -o "$OUT" $LDLIBS

# ---- dev artifacts: single static lib + flat public header set ----
# libtphp.a = engine + vendored deps objects (uv/mbedtls/mimalloc/pcre2/
# miniz/ir/asm).
# headers: src/*.h + ext/*/*.h are flat includes (no path refs, no name
# clashes) + libuv uv.h/uv/ tree (tio.h pulls <uv.h> transitively).
echo "[pkg] build/libtphp.a + build/include/"
INC_OUT=build/include
rm -rf "$INC_OUT"
mkdir -p "$INC_OUT"
cp src/*.h "$INC_OUT"/
for d in ext/*/; do
  for f in "$d"*.h; do
    [ -e "$f" ] || continue
    cp "$f" "$INC_OUT"/
  done
done
cp deps/libuv/include/uv.h "$INC_OUT"/
cp -R deps/libuv/include/uv "$INC_OUT"/
AR_BIN=ar
if command -v llvm-ar >/dev/null 2>&1; then AR_BIN=llvm-ar; fi
rm -f build/libtphp.a
"$AR_BIN" rcs build/libtphp.a "$OBJ"/*.o

echo "Build OK: $OUT"
