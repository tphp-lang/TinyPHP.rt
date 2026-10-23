#!/usr/bin/env bash
# Incremental build: compile the given .c files, then relink. No dep rebuild.
set -e
cd /c/project/tphp/TinyPHP.rt

INC="-Isrc -Ideps/libuv/include -Ideps/libuv/src -Ideps/mimalloc/include -Ideps/mimalloc/src -Ideps/mbedtls/include -Ideps/mbedtls/library -Ideps/libffi/include -Ideps/miniz -Iext/ffi -Iext/net -Iext/io -Iext/worker -Iext/json -Iext/serialize -Iext/pkg -Iext/builtin -Iext/standard"
DEFS='-DTPHP_TARGET="x86_64-w64-windows-gnu"'
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
