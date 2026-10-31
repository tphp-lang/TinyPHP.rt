#!/usr/bin/env bash
# Incremental build: compile the given .c files, then relink. No dep rebuild.
# Flags must stay in sync with build.cmd (Windows canonical). TFunc 变更后
# 需全量重编 src+ext（deps 对象不受影响）——本脚本只适合单文件热修。
set -e
cd /c/project/tphp/TinyPHP.rt

# -Ideps/ir 在 libffi 前?否——libffi 的 ffi.h 必须先于 ext/ffi 的同名头
# （build.cmd 同序）；-Ideps/ir 供 jit.c（deps/ir/ir.h）。
INC="-Isrc -Ideps/ir -Ideps/libuv/include -Ideps/libuv/src -Ideps/mimalloc/include -Ideps/mimalloc/src -Ideps/mbedtls/include -Ideps/mbedtls/library -Ideps/libffi/include -Ideps/miniz -Ideps/pcre2 -Iext/ffi -Iext/net -Iext/io -Iext/worker -Iext/json -Iext/serialize -Iext/pkg -Iext/builtin -Iext/standard -Iext/pcre"
# -DTPHP_JIT 必须与 build.cmd 一致：缺它编译出无 JIT 的 src_jit.o/src_vm.o，
# 链接报 tphp_osr_backedge/jit_entry 符号缺失（2026-10-31 实测两次踩坑）。
DEFS='-DTPHP_TARGET="x86_64-w64-windows-gnu" -DTPHP_JIT'
CFLAGS="-O2 -std=c11 -Wall -Wextra -g -fno-omit-frame-pointer -D_GNU_SOURCE -DTPHP_USE_MIMALLOC"

for f in "$@"; do
  base=$(basename "$f" .c)
  echo "[cc] $f"
  clang $CFLAGS $DEFS $INC -c "$f" -o "build/obj/src_${base}.o"
done

echo "[ld] build/tphp.exe"
clang build/obj/*.o deps/libffi/lib/libffi.a -o build/tphp.exe \
  -ldbghelp -lws2_32 -ladvapi32 -luser32 -liphlpapi -lpsapi -lshell32 \
  -lole32 -loleaut32 -luuid -lbcrypt -lsynchronization -luserenv -lcrypt32
echo "[ok]"
