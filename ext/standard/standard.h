/*
 * TinyPHP.rt - ext/standard/standard.h - PHP ext/standard 函数子集
 *
 * 对拍基准：本机 php-8.5.1（C:\env\php-8.5.1-nts-Win32-vs17-x64），
 * 边界语义（offset 越界 ValueError、空 needle、nl2br 换行序列、
 * basename/dirname 路径规则）以 8.5.1 实测为准。
 * hebrevc 为 PHP 8.0 removed，不做；hebrev 后置（bidi 语义复杂）。
 */
#ifndef TPHP_EXT_STANDARD_H
#define TPHP_EXT_STANDARD_H

#include "opcode.h"
#include "value.h"

/* string */
TVal bi_strtoupper(VM *vm, TVal *args, int n);
TVal bi_strtolower(VM *vm, TVal *args, int n);
TVal bi_strpos(VM *vm, TVal *args, int n);
TVal bi_stripos(VM *vm, TVal *args, int n);
TVal bi_strrpos(VM *vm, TVal *args, int n);
TVal bi_strripos(VM *vm, TVal *args, int n);
TVal bi_strrev(VM *vm, TVal *args, int n);
TVal bi_nl2br(VM *vm, TVal *args, int n);

/* path */
TVal bi_basename(VM *vm, TVal *args, int n);
TVal bi_dirname(VM *vm, TVal *args, int n);

#endif /* TPHP_EXT_STANDARD_H */
