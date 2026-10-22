/*
 * TinyPHP.rt - ext/builtin/builtin.h - 共享 builtin 辅助（跨扩展复用）
 *
 * need_str/need_int：参数类型校验（strict，非目标类型 TypeError，文案
 * 对齐 PHP "must be of type X, Y given"）。
 * str_find：子串查找（from 起、ci 大小写不敏感、reverse 从尾向前）。
 * 供 ext/standard 等扩展使用——不要再把这些改回 static。
 */
#ifndef TPHP_EXT_BUILTIN_H
#define TPHP_EXT_BUILTIN_H

#include "opcode.h"
#include "value.h"

TStr *need_str(VM *vm, TVal *v, const char *fn, const char *arg);
int64_t need_int(VM *vm, TVal *v, const char *fn, const char *arg);
int64_t str_find(TStr *hay, TStr *needle, int64_t from, bool ci, bool reverse);

#endif /* TPHP_EXT_BUILTIN_H */
