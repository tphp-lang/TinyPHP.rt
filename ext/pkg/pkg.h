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

/* ---- ext C 扩展（PLAN/ext-packages.md §2.3 脚手架） ---- */
/* <tphp_root> = tphp 可执行文件所在目录（g_self_exe dirname）；
 * 无法定位（self exe 未设）返回 false。 */
bool tphp_pkg_ext_root(char *buf, size_t cap);
/* tphp new <name> --ext：脚手架 → <tphp_root>/ext/<name>/（mod.json 契约 +
 * src/<pkg>.c + include/<pkg>.h，不写任何项目清单）。name 双段 vendor/pkg。
 * force：目标已存在时覆盖脚手架。0 成功；1 名字非法；2 IO。 */
int tphp_pkg_new_ext(const char *name, bool force, char *err, size_t errcap);

/* ---- ext 命令组（PLAN/ext-packages.md §4）----
 * 落点 <tphp_root>/ext/<name>；proj_dir 有 mod.json 且非 type=ext = 项目
 * 模式（写回 ext[]），否则只装。rc：0 成功；1 参数/校验；2 IO/清单。
 * --add <source> [ref]：物化 + 校验（type=ext / 双段名 / 覆盖三查）。 */
int tphp_pkg_ext_add(const char *proj_dir, const char *source, const char *ref,
                     char *err, size_t errcap);
/* 无 source = 按项目 ext[] 补齐缺失（§4.2 sync）。out_summary 可 NULL；
 * 非 NULL 收逐项结果（tphp_pkg_free 释放）。 */
int tphp_pkg_ext_sync(const char *proj_dir, char **out_summary,
                      char *err, size_t errcap);
/* --update [name]：按 url/ref 重下（name 省略 = 全部；需项目 ext[]）。 */
int tphp_pkg_ext_update(const char *proj_dir, const char *name,
                        char **out_summary, char *err, size_t errcap);
/* --remove <name>：删目录 + 项目 ext[] 移除（先校验 mod.json name 对账）。 */
int tphp_pkg_ext_remove(const char *proj_dir, const char *name,
                        char *err, size_t errcap);
/* --list：表头 + 每行 name/source/built-in/project（调用方负责打印）。 */
int tphp_pkg_ext_list(const char *proj_dir, char **out,
                      char *err, size_t errcap);
/* --relink：全量重链（§5 编译管线未实现时返回 2）。 */
int tphp_pkg_ext_relink(char *err, size_t errcap);
/* 释放本模块返回的可变缓冲（list / summary）。 */
void tphp_pkg_free(void *p);

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

/* lifecycle scripts（PLAN/pkg-manager.md §2.1 scripts）：
 * self_exe = 当前 tphp 可执行文件绝对路径（PHP 钩子用）；main 启动时设置。 */
void tphp_pkg_set_self_exe(const char *path);
/* CLI --ignore-scripts 或环境 TPHP_PKG_SCRIPTS=0 → 跳过全部钩子 */
void tphp_pkg_set_ignore_scripts(bool on);

/* build 自动打包 vendor 前的 integrity 卡口：对照 mod.lock 快照逐文件
 * 校验已装包（无 lock / 旧 lock 无快照 = 放行）。0 通过；2 不匹配。 */
int tphp_pkg_verify_vendor(const char *proj_dir, char *err, size_t errcap);

#endif /* TPHP_PKG_H */
