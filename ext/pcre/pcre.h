/*
 * TinyPHP.rt - pcre.h - preg_* (PLAN/preg.md)
 */
#ifndef TPHP_EXT_PCRE_H
#define TPHP_EXT_PCRE_H

#include "value.h"

struct VM;
TVal bi_preg_match(struct VM *vm, TVal *args, int n);
TVal bi_preg_match_all(struct VM *vm, TVal *args, int n);
TVal bi_preg_replace(struct VM *vm, TVal *args, int n);
TVal bi_preg_split(struct VM *vm, TVal *args, int n);
TVal bi_preg_grep(struct VM *vm, TVal *args, int n);
TVal bi_preg_last_error(struct VM *vm, TVal *args, int n);
TVal bi_preg_last_error_msg(struct VM *vm, TVal *args, int n);
TVal bi_preg_quote(struct VM *vm, TVal *args, int n);

#endif /* TPHP_EXT_PCRE_H */
