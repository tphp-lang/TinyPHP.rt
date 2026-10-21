@echo off
rem TinyPHP.rt build script (Windows, clang). ASCII only.
rem Usage: build.cmd
setlocal

set CC=clang
set OUT=build\tphp.exe
set OBJ=build\obj

rem ---- flags ----
set CFLAGS=-O2 -std=c11 -g -fno-omit-frame-pointer
set CFLAGS_US=-O2 -std=c11 -Wall -Wextra -g -fno-omit-frame-pointer -D_GNU_SOURCE -DTPHP_USE_MIMALLOC
set INC=-Isrc -Ideps\libuv\include -Ideps\libuv\src -Ideps\mimalloc\include -Ideps\mimalloc\src -Ideps\mbedtls\include -Ideps\mbedtls\library -Ideps\libffi\include -Ideps\miniz
set DEFS=-DTPHP_TARGET="\"x86_64-w64-windows-gnu\""

if not exist build mkdir build
if not exist %OBJ% mkdir %OBJ%

echo [1/4] compiling vendored deps (libuv, mimalloc)...
for %%f in (deps\libuv\src\*.c) do %CC% -w %CFLAGS% %INC% -c "%%f" -o "%OBJ%\uv_%%~nf.o" || goto :fail
for %%f in (deps\libuv\src\win\*.c) do %CC% -w %CFLAGS% %INC% -c "%%f" -o "%OBJ%\uvw_%%~nf.o" || goto :fail
rem mimalloc: explicit list from its CMakeLists (alloc-override.c / free.c are internal includes)
rem MI_MINGW_UCRT64: official cmake defines this under MSYSTEM=UCRT64; without it the
rem prim.c constructor AND tls callback both run process init twice (assert in options.c)
set MI_SRCS=alloc.c alloc-aligned.c alloc-posix.c arena.c bitmap.c heap.c init.c libc.c options.c os.c page.c page-map.c random.c sample-guarded.c sample-profile.c stats.c subproc.c theap.c threadlocal.c
set MI_DEFS=-DMI_STATIC_LIB -DMI_MINGW_UCRT64=1
for %%f in (%MI_SRCS%) do %CC% -w %CFLAGS% %MI_DEFS% %INC% -c deps\mimalloc\src\%%f -o "%OBJ%\mi_%%~nf.o" || goto :fail
for %%f in (deps\mimalloc\src\prim\*.c) do %CC% -w %CFLAGS% %MI_DEFS% %INC% -c "%%f" -o "%OBJ%\mip_%%~nf.o" || goto :fail

echo [2/5] compiling fiber asm (x86_64 windows)...
for %%f in (deps\asm\*_ms_pe_gas.S) do %CC% -w %CFLAGS% -c "%%f" -o "%OBJ%\asm_%%~nf.o" || goto :fail

echo [3/5] compiling mbedtls...
for %%f in (deps\mbedtls\library\*.c) do %CC% -w %CFLAGS% %INC% -c "%%f" -o "%OBJ%\mb_%%~nf.o" || goto :fail

echo [3b/5] compiling miniz...
%CC% -w %CFLAGS% %INC% -c deps\miniz\miniz.c -o "%OBJ%\miniz.o" || goto :fail

echo [4/5] compiling tphp sources...
for %%f in (src\*.c) do %CC% %CFLAGS_US% %DEFS% %INC% -c "%%f" -o "%OBJ%\src_%%~nf.o" || goto :fail

echo [5/6] linking...
%CC% %OBJ%\*.o deps\libffi\lib\libffi.a -o %OUT% -ldbghelp -lws2_32 -ladvapi32 -luser32 -liphlpapi -lpsapi -lshell32 -lole32 -loleaut32 -luuid -lbcrypt -lsynchronization -luserenv -lcrypt32 || goto :fail

rem M6: two-stage - if lib\*.php exists, pack into bundle_data.c and relink
if not exist lib goto :done
dir /b lib\*.php >nul 2>&1
if errorlevel 1 goto :done
echo [6/6] packing lib\*.php into bundle_data.c...
%OUT% phpc lib -o src\bundle_data.c || goto :fail
%CC% %CFLAGS_US% %DEFS% %INC% -c src\bundle_data.c -o "%OBJ%\src_bundle_data.o" || goto :fail
%CC% %OBJ%\*.o deps\libffi\lib\libffi.a -o %OUT% -ldbghelp -lws2_32 -ladvapi32 -luser32 -liphlpapi -lpsapi -lshell32 -lole32 -loleaut32 -luuid -lbcrypt -lsynchronization -luserenv -lcrypt32 || goto :fail

:done
echo Build OK: %OUT%
exit /b 0

:fail
echo Build FAILED
exit /b 1
