/*
 * TinyPHP.rt - pkg.h - built-in package manager (PLAN/pkg-manager.md)
 *
 * 不依赖外部 git：包来源为 HTTP(S) 压缩包直链 / 本地路径（zip 或目录）。
 * 阶段 A：mod.json 清单 + 项目骨架（tphp new）+ vendor/autoload.php 生成。
 */
#ifndef TPHP_PKG_H
#define TPHP_PKG_H

#include <stdbool.h>
#include <stddef.h>

/* json.c C-level API（无 VM、静默失败；pkg 专用） */
struct TVal;
bool tphp_json_decode_c(const char *src, size_t len, struct TVal *out);

/* 名字校验：app = 单段 [a-z0-9][a-z0-9_-]*；lib = <seg>/<seg>。
 * 非法时 *why 指向原因（静态串，可为 NULL）。 */
bool tphp_pkg_valid_name(const char *name, bool is_lib, const char **why);

/* 在 dir 生成骨架（dir 必须不存在）。返回 0 成功；1 名字非法；2 IO 失败。
 * 失败时向 err 写入 UTF-8 说明（err 可为 NULL）。 */
int tphp_pkg_new(const char *dir, const char *name, bool is_lib,
                 char *err, size_t errcap);

/* 重新生成 proj_dir/vendor/autoload.php（根项目 + vendor 下各包清单合并） */
int tphp_pkg_gen_autoload(const char *proj_dir, char *err, size_t errcap);

/* HTTP(S) 拉取回调：main.c 注册（复用 http_get + run_source）。
 * 成功返回 0，*out = malloc 缓冲（调用方 free）。 */
typedef int (*TphpPkgFetchFn)(const char *url, char **out, size_t *out_len,
                              char *err, size_t errcap);
void tphp_pkg_set_fetch(TphpPkgFetchFn fn);

/* tphp add：source 为本地目录 / 本地 zip / HTTP(S) zip URL；
 * ref 可为 NULL（记录用；HTTP URL 本身即版本锚点）。
 * proj_dir 必须含 mod.json。返回 0 成功；1 参数；2 IO/网络。 */
int tphp_pkg_add(const char *proj_dir, const char *source, const char *ref,
                 char *err, size_t errcap);

/* tphp remove：name 为包 ID（foo/bar）。清理不再可达的传递依赖。 */
int tphp_pkg_remove(const char *proj_dir, const char *name,
                    char *err, size_t errcap);

/* tphp install：按 mod.lock 恢复缺失包（vendor 已存在则跳过，离线友好）；
 * 无 lock 时从 mod.json deps 全量解析。完成后重写 lock + autoload。
 * force：已装包 integrity 校验不通过时删除重装（--force）；否则 fail-closed。 */
int tphp_pkg_install(const char *proj_dir, bool force,
                     char *err, size_t errcap);

/* build 自动打包 vendor 前的 integrity 卡口：对照 mod.lock 快照逐文件
 * 校验已装包（无 lock / 旧 lock 无快照 = 放行）。0 通过；2 不匹配。 */
int tphp_pkg_verify_vendor(const char *proj_dir, char *err, size_t errcap);

#endif /* TPHP_PKG_H */
