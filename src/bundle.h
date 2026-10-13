/*
 * TinyPHP.rt - bundle.h - M6 内嵌 PHP 模块（单 exe 打包）
 *
 * phpc 把 lib\*.php 的源码编成 C 数组（src/bundle_data.c）；
 * 运行期 require 先查 bundle 表，命中则用内嵌源码 parse+inline，
 * 未命中回落文件系统。源码内嵌复用全部编译器，opcode 序列化后置。
 */
#ifndef TPHP_BUNDLE_H
#define TPHP_BUNDLE_H

#include <stddef.h>
#include <stdbool.h>

typedef struct TBundle {
    const char *name;          /* 模块名（不含路径/扩展名）："std" "util" */
    const char *src;           /* PHP 源码（NUL 结尾） */
    size_t len;
} TBundle;

/* 由 src/bundle_data.c 定义；无打包时为空表 */
extern const TBundle tphp_bundles[];
extern const int tphp_bundle_count;

/* 按名查 bundle（精确匹配 name）；未命中返回 NULL */
const TBundle *tphp_bundle_find(const char *name);

/* 从路径提取模块名：'lib/util.php' → 'util'；'std' → 'std' */
void tphp_bundle_name_of(const char *path, char *out, size_t cap);

#endif /* TPHP_BUNDLE_H */
