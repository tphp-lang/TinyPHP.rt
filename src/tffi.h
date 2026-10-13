/*
 * TinyPHP.rt - tffi.h - M5.5 FFI 对外接口（vm.c / main.c 用）
 * 注意：不能叫 ffi.h —— 会顶掉 libffi 的系统头（-Isrc 优先级教训同 io.h）。
 */
#ifndef TPHP_TFFI_H
#define TPHP_TFFI_H

#include "value.h"

struct VM;
struct TClass;

/* 内部类注册（编译前，幂等） */
void tphp_ffi_init(void);

/* op_init_mcall：FFI 对象上未命中的方法 → 尝试按 cdef 符号分发。
 * 成功时 rec 已填好（is_builtin/ffi_fn/this_obj），返回 true。 */
bool tphp_ffi_try_mcall(struct VM *vm, struct TObj *o, struct TStr *mname, void *rec);

/* op_do_fcall：rec->ffi_fn 调用路径 */
TVal tphp_ffi_call_fn(struct VM *vm, void *fn, TVal *args, int n);

/* CData 属性/下标钩子：命中返回 true 且 *out 为 +1 值 */
bool tphp_ffi_fetch_prop(struct VM *vm, struct TObj *o, struct TStr *name, TVal *out);
bool tphp_ffi_assign_prop(struct VM *vm, struct TObj *o, struct TStr *name, TVal val);
bool tphp_ffi_fetch_dim(struct VM *vm, TVal base, TVal key, TVal *out);
bool tphp_ffi_assign_dim(struct VM *vm, TVal base, TVal key, TVal val);

/* 类身份查询 */
bool tphp_ffi_is_cdata_cls(struct TClass *cls);
bool tphp_ffi_is_ffi_cls(struct TClass *cls);

/* 内部：包一层 CData（ffi.c 内部使用也经此符号） */
TVal tphp_ffi_wrap_ptr(void *ptr, void *type, bool owned, void *scope);

#endif /* TPHP_TFFI_H */
