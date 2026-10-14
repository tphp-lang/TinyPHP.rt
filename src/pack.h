/*
 * TinyPHP.rt - pack.h - M6.1 原生资产打包（PLAN/03 §6.1，跨平台）
 *
 * 构建侧：tphp build app.php --with <path>... -o app
 *   收集非 .php 文件 → miniz zip（含 manifest.json + 可选 __main.php）
 *   嵌入方式：
 *     Windows: PE RCDATA 资源 TPHP_BUNDLE + TPHP_MAIN
 *     Linux:   ELF section .tphp_bundle（objcopy；失败回落文件尾 footer）
 *     macOS:   文件尾 footer 容器（[zip][u64 size][magic]）
 * 运行侧：main() 最早期 tphp_pack_startup()
 *   zip 常驻内存：数据资产经 tphp_pack_* 直读（io 层 tphp://asset/ 前缀）
 *   仅 manifest 中 type=dll 的条目按等效相对路径提取到 exe 同级
 *   （<exedir>/<relpath>；已存在且 sha1 与清单一致则跳过）
 *   Unix 上 __main.php 作为嵌入主脚本（无 CLI 参数时执行）
 *
 * 路径约定：接口一律 UTF-8；OS 边界在 pack.c 内转宽字符（Windows）。
 */
#ifndef TPHP_PACK_H
#define TPHP_PACK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 构建：把 assets 打进目标可执行文件（调用前已复制）。
 * exe_path: 目标可执行文件（UTF-8）
 * main_php: 主脚本源码与长度（可 NULL）
 * with_paths: --with 列表（UTF-8）；with_n 个数
 * 返回 0 成功 */
int tphp_pack_exe(const char *exe_path,
                  const char *main_php, size_t main_php_len,
                  char **with_paths, int with_n);

/* 运行期：定位嵌入 zip → 常驻内存 → 提取 DLL（无则跳过）→ 配置搜索路径。
 * out_main: 若存在嵌入主脚本且 *out_main 为空，*out_main = malloc 源码
 *           （调用方 free），返回 true */
bool tphp_pack_startup(char **out_main, size_t *out_main_len);

/* ---- 运行期 bundle 访问（未打包时 has=false，read/stat=false） ---- */

/* zip 是否常驻（进程内打包应用） */
bool tphp_pack_has_bundle(void);

/* DLL 落盘根 = exe 所在目录（UTF-8 绝对路径；无待提取 DLL 时返回 NULL）。
 * FFI 用它把包内相对路径解析成等效绝对路径加载。 */
const char *tphp_pack_dll_root(void);

/* 读 zip 内文件到堆（*out = malloc(len+1)，已 NUL 结尾）。rel 用 '/'。 */
bool tphp_pack_read(const char *rel, void **out, size_t *out_len);

/* 查 zip 内文件大小；out_size 可为 NULL */
bool tphp_pack_stat(const char *rel, size_t *out_size);

/* 列 zip 内 dir_rel（"" = 根）下一级名字（含目录项，不含 '.'/'..'）。
 * 返回个数；*names = malloc 数组（每项 malloc），调用方逐项 free。 */
int tphp_pack_list(const char *dir_rel, char ***names);

#endif /* TPHP_PACK_H */
