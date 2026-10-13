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
 *   查找嵌入 zip → 逐条目 sha1 校验 → 原子释放到 <exedir>/runtime/lib
 *   Unix 上 __main.php 作为嵌入主脚本（无 CLI 参数时执行）
 */
#ifndef TPHP_PACK_H
#define TPHP_PACK_H

#include <stdbool.h>
#include <stddef.h>

/* 构建：把 assets 打进目标可执行文件（调用前已复制）。
 * exe_path: 目标可执行文件
 * main_php: 主脚本源码与长度（可 NULL）
 * with_paths: --with 列表；with_n 个数
 * 返回 0 成功 */
int tphp_pack_exe(const char *exe_path,
                  const char *main_php, size_t main_php_len,
                  char **with_paths, int with_n);

/* 运行期：若自身带嵌入 bundle 则释放到 runtime/lib 并配置搜索路径。
 * out_main: 若存在嵌入主脚本，*out_main = malloc 源码（调用方 free），返回 true */
bool tphp_pack_startup(char **out_main, size_t *out_main_len);

#endif /* TPHP_PACK_H */
