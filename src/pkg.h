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

#endif /* TPHP_PKG_H */
