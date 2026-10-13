/*
 * TinyPHP.rt - bundle.c - M6 bundle 查表
 */
#include "bundle.h"
#include <string.h>
#include <stdio.h>

/* 默认空表（未打包时 bundle_data.c 提供真表；此处兜底弱符号不现实，
 * 直接由 bundle_data.c 始终定义——phpc 至少生成空表）。 */
/* bundle_data.c 定义 tphp_bundles / tphp_bundle_count */

const TBundle *tphp_bundle_find(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < tphp_bundle_count; i++) {
        if (strcmp(tphp_bundles[i].name, name) == 0)
            return &tphp_bundles[i];
    }
    return NULL;
}

void tphp_bundle_name_of(const char *path, char *out, size_t cap) {
    /* 取最后一段，去掉目录与 .php 扩展名 */
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    size_t n = strlen(base);
    if (n > 4 && (strcmp(base + n - 4, ".php") == 0 || strcmp(base + n - 4, ".PHP") == 0))
        n -= 4;
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = 0;
}
