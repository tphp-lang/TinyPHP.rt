# TinyPHP.rt 项目记忆（长期）

- 定位：PHP 8 **解释型运行时**（lex→parse→compile→自有 opcode→VM 解释），非 AOT/JIT；与 TinyPHP（AOT 编译器）代码零共享、定位互补。CLI 名 `tphp`，当前 0.0.1。
- 完成态（2026-10-14 实测）：M0~M6 + M5.5 FFI + M6.1 跨平台打包器全部关闭；tests/run.ps1 **108 passed 0 failed**；CI 5 平台（Win/Linux x64+aarch64/macOS arm64+x64）。
- 核心设计：TExecCtx 每协程 VM 状态整块切换（fiber 抄 php-src Zend/asm）；prog freeze 后字节码跨线程只读共享；异常表驱动 + longjmp unwind；Worker = 独立 TPhpRuntime 全 TLS 隔离 + uv_mutex mailbox + serialize 克隆；bundle 用**源码内嵌**（非 opcode 序列化，TOp 含 handler 指针不可序列化）；pack.c = PE RCDATA / ELF section / macOS footer 三平台资产打包。
- 环境坑（本项目实测）：src 下自定义头禁叫 io.h/ffi.h（顶掉系统头）；.cmd 纯 ASCII；ffi_prep_cif 不拷贝 arg_types 数组必须堆分配；PS5 写无 BOM UTF-8；build.log 为 UTF-16 需 iconv；PowerShell 传参剥引号用 rsp 响应文件。
- 用户边界：严格前后端/文档一致性；不为修而修；测试必须真实跑（禁改测试假通过）。
- OOP 审计（2026-10-15，~70 探针 vs PHP 8.6-dev 对拍）：主体与 PHP 逐字节一致；**P0：①跨帧 catch 断裂（调用是 try 体最后一条 op 时接不住，compile.c ex 表 end_pc 差一 + vm.c:1162 压帧前推 pc；修法 end_pc=end+1）②__destruct 永不触发**；P1：typed 属性全线 Parse error（与文档矛盾）、用户异常类不可 extends/catch（TExClass 与 TClass 两套系统）、Closure 类缺失；P2：nullsafe/new 命名参数/方法命名参数/方法 first-class callable 均为 "lands in M3" 占位桩（PLAN/01 §11 示例自相矛盾）、类常量表达式全拒、trait 常量不导入、子类静态属性重声明失效、数组/字符串 callable ❌；P3：override 签名/私有 ctor 不校验、接口方法体被接受、ValueError 缺失。测试盲区：现有套件只测同帧 catch、无 typed 属性/析构测试。

## 2026-10-15 OOP 审计修复（P0+P1 全闭环，116/116）
- P0-1 跨帧异常：S_TRY 三处 end_pc=end+1（缩进不一，replace_all 曾漏一处）
- P0-2 __destruct：**op_new_user rc++ 未抵消是总根因**（每对象恒漏一引用）；析构=引擎引用+DTORED+复活判断(rc>1)+GC defer 两段式+收尾快照遍历；GC 补 OBJ 属性边与全 kind 释放；sort/rsort/array_unshift 曾是空操作（写回 args 副本）
- P1-1 类型声明：parse_type_decl 归一化 "?int"/"a\b\c"；tphp_type_accepts（strict+int→float宽化+self/static/parent/类名/closure 伪类型）；五个检查点（recv/recv_variadic/return/assign_prop/assign_sprop）；`C::$x=5` 曾是 Parse error（is_lvalue 过窄）
- P1-2 用户异常类：双世界名字匹配（TExObj 快路 + wrap 字段包装用户 TObj）；exclass.c 注册 16+1 内置异常 TClass（master 表，worker 靠克隆）；**enum from(非法值) 曾静默返回 null**（op-1-n_args 反查 INIT 公式在带参时指错→改存 rec->magic_name）
- P1-3 Closure：闭包 $this 自动捕获（uses 双侧同序+指针去重）；TClosure.scope 重绑（bindTo/bind scope 参、call() 用新对象类）；tphp_vm_call_callable 曾 called_cls=NULL；链式调用 f()() 曾 Parse error
- 环境注意：main.c 等处中文注释经 Edit 写入是合法 UTF-8（clang 输出乱码只是控制台显示）；exclass 走 master 注册后 worker 不可再直接 init（clone 重复 fatal）
- ~~已知待办~~ **进度已全清（2026-10-16 核对）**：TODO-P2-P3.md 已于 a8fb0b2 删除，P2/P3 全部关闭或迁至 **PLAN/01 §10（行为差异权威源，18 条）**；基线 **135 passed / 0 failed**，HEAD=fca98d1。后续新缺口一律记 PLAN/01 §10。
- 架构锚点：惰性类常量=TClass.cthunks+compile_thunk(cls)；readonly 写入=TProp.decl 声明类判定；**继承静态属性共享=TProp.decl 共享父槽（重声明才分槽）**；override 签名=class.c 类型格；parent::__construct 帧作用域=VMFrame.vis_scope（与 called_cls 分离）；自工厂=compile_class_decl 壳建后提前注册；try 内声明=pass 系递归 S_TRY（if/循环不递归）；`$obj::$x`=OSPROP 三 handler；测试运行器=cmd /c 引号剥离需 @ 前缀+rc&0xFF
