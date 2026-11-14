# bug/

主项目缺陷记录（主窗口会话认领分析）。文件名 = `YYYY-MM-DD-slug.md`；
认领修复后将「状态」改为"已修"并回填 commit。

| 日期 | 文件 | 标题 | 状态 | 来源 |
|---|---|---|---|---|
| 2026-11-14 | [2026-11-14-strxor-both-strings-returns-int.md](2026-11-14-strxor-both-strings-returns-int.md) | `^`/`&`/`\|`/`~` 两操作数均为字符串时返回 int（恒 0/-1），非 PHP 逐字节字符串语义 | 已修（value.c both-strings 分支 + test-bitwise-str 22 行；^ MIN 规格更正；commit 待回填） | teb WS 帧层收尾会话 |
| 2026-11-14 | [2026-11-14-winpthread-dll-clock_gettime64.md](2026-11-14-winpthread-dll-clock_gettime64.md) | tphp.exe 启动期依赖 clang64 的 libwinpthread-1.dll（仅 clock_gettime64 一个符号），PATH 无 clang64 即 0xC0000135 | 已修（build.cmd ×2 + pkg.c ext_engine_libs `-static`；ext relink 面实证；commit 待回填） | teb WS 帧层收尾会话 |
