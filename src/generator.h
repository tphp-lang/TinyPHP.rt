/*
 * TinyPHP.rt - generator.h - PHP 8 generators (PLAN/generators.md)
 * Generator 对象持挂起的 VMFrame（帧保存模型，PHP 同构）。
 */
#ifndef TPHP_GENERATOR_H
#define TPHP_GENERATOR_H

#include "value.h"

struct VM;
struct VMFrame;
struct TOp;

/* 注册 Generator 内部类（编译前，幂等） */
void tphp_generator_init(void);

/* OP_GENERATOR_CREATE：把当前（生成器函数）帧打包成 Generator 对象。
 * 对象 +1 引用交给调用方；帧 prev 暂不改（handler 负责摘链）。 */
TVal tphp_generator_pack(struct VM *vm, struct VMFrame *f);

/* OP_YIELD：暂停当前生成器帧（value/key/send_slot/state），摘链交还驱动方。
 * 值由 vm.c 的 op_yield 用 rdv 读出后传入（rdv 是 vm.c 私有）。
 * 帧 pc 已由本函数推进到 yield 之后。 */
void tphp_generator_pause(struct VM *vm, struct VMFrame *f, TVal v, TVal k,
                          bool has_key, uint32_t send_slot, bool has_send);

/* 函数 RETURN 且帧是生成器帧：存 retval、关闭并销毁帧。成功返回 true
 * （op_return 应立即 return 0）。 */
bool tphp_generator_finish(struct VM *vm, TVal ret);

/* 驱动：next/send/foreach 用。gen 为 Generator 对象。
 * has_send 时 sendv 写入最近 yield 的 dst。CLOSED 静默返回 0。 */
int tphp_generator_resume(struct VM *vm, TVal gen, TVal sendv, bool has_send);

/* foreach 钩子：reset=驱动到首个 yield（false=非 Generator）；
 * fetch 输出当前 key/value（+1）并推进（*more=false 时 key/val 无效）。 */
bool tphp_generator_fe_reset(struct VM *vm, TVal v);
bool tphp_generator_fe_fetch(struct VM *vm, TVal gen, TVal *key, TVal *val,
                             bool *more);

/* native_dtor：未关闭生成器的帧直接销毁（不跑 finally——已知差异 §6） */
void tphp_generator_dtor(TObj *o);

bool tphp_generator_is_obj(TVal v);

#endif /* TPHP_GENERATOR_H */
