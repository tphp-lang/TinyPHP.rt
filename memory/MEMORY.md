# TinyPHP.rt 项目记忆（长期）

- 定位：PHP 8 **解释型运行时**（lex→parse→compile→自有 opcode→VM 解释），非 AOT/JIT；与 TinyPHP（AOT 编译器）代码零共享、定位互补。CLI 名 `tphp`，当前 0.0.1。
- 完成态（2026-10-25 实测）：M0~M6 + M5.5 FFI + M6.1 打包器全关；包管理五阶段 A–E 全关；**FFI::cast 拒裸 int**；ext/standard（对拍 php-8.5.1）；**FFI 回调 v1+v2**；生成器 yield v1；preg_* 全批（vendor pcre2）；C 批语法；解释器优化 L1 帧池化/L2 packed 读/L3 键 intern/string .= in-place/L5 惰性桶同步（push -74%）；**IR baseline JIT v1（PLAN/ir-jit.md）：fib 13.2ms vs PHP 追踪 JIT 10.9ms = 1.2×（M1 达成）**；tests/run.ps1 **218 passed 0 failed**；CI 5 平台。
- **性能目标（2026-10-24 用户拍板）= 持平或超越开 tracing JIT 的 PHP**。唯一通路 = PLAN/perf-roadmap §5 自研 IR JIT（vendor dstogov/ir → `deps/ir/`，与 Zend 8.4+ 同后端）；**spike 已通**：fib(28) IR-JIT 1.2ms vs Zend tracing JIT 10.9ms（9×，65 字节机器码）——后端质量验证，胜负在 opcode→IR lowering。解释器优化只做地板与 JIT 未命中下限。三方差距表在 perf-roadmap §1.1 + doc/testing.md；**跨运行时 bench 禁用"常量键+无副作用求和"形态**（Zend optimizer 进程间双态折叠，kv-read 已改变量键）；IR 对象隔离在 `build/obj_ir/`（勿进 build/obj/），无 libgcc 需 tools/chkstk_stub.S。
- 核心设计：TExecCtx 每协程 VM 状态整块切换（fiber 抄 php-src Zend/asm）；prog freeze 后字节码跨线程只读共享；异常表驱动 + longjmp unwind；Worker = 独立 TPhpRuntime 全 TLS 隔离 + uv_mutex mailbox + serialize 克隆；bundle 用**源码内嵌**（非 opcode 序列化，TOp 含 handler 指针不可序列化）；pack.c = PE RCDATA / ELF section / macOS footer 三平台资产打包。
- 环境坑（本项目实测）：src 下自定义头禁叫 io.h/ffi.h（顶掉系统头）；.cmd 纯 ASCII；ffi_prep_cif 不拷贝 arg_types 数组必须堆分配；PS5 写无 BOM UTF-8；build.log 为 UTF-16 需 iconv；PowerShell 传参剥引号用 rsp 响应文件。**PS5 批量改含中文源文件禁用 Get-Content/Set-Content**（默认 ANSI(GBK) 读写会把全文件 UTF-8 注释写成乱码——vm.c 曾中招，git checkout 重放恢复）；用 .NET `[IO.File]::ReadAllText/WriteAllText($f, [Text.UTF8Encoding]::new($false))` 或 Edit 工具。
- HTTPS 信任面：内嵌 Mozilla `cacert.c` + **Windows 额外并入系统 Root 证书库**（net.c `tls_cacert_load_win_store`，build.cmd 加 `-lcrypt32`）；github.com 在国内用户机器上可能被加速器类工具 MITM（假 CA 在系统库、浏览器正常），tphp 拒绝是正确行为，握手失败会打 verify 标志位明细（永久诊断）。
- `tphp add` 源形态（2026-10-21）：**GitHub 仓库 URL**（`:ref` 后缀或 CLI ref → `zipball/<ref>`；无 ref → GitHub 匿名 API 解析最新发布并**固化进 mod.json ref**，releases/latest → tags semver → 默认分支回退）/ zip 直链 / 本地路径；无 git。mod.json 存剥后缀仓库 URL。vendor 覆盖前 fail-closed 安全检查（`vendor_overwrite_blocked`）。**教训**：sched_cleanup 必须复位 `g_loop_inited`（uv_loop_close 后二次 tphp_vm_run 曾 0xC0000005）；空 ref 判断不能用 `!ptr`（栈数组空串指针非 NULL）。
- 用户边界：严格前后端/文档一致性；不为修而修；测试必须真实跑（禁改测试假通过）。
- OOP 审计（2026-10-15，~70 探针 vs PHP 8.6-dev 对拍）：主体与 PHP 逐字节一致；**P0：①跨帧 catch 断裂（调用是 try 体最后一条 op 时接不住，compile.c ex 表 end_pc 差一 + vm.c:1162 压帧前推 pc；修法 end_pc=end+1）②__destruct 永不触发**；P1：typed 属性全线 Parse error（与文档矛盾）、用户异常类不可 extends/catch（TExClass 与 TClass 两套系统）、Closure 类缺失；P2：nullsafe/new 命名参数/方法命名参数/方法 first-class callable 均为 "lands in M3" 占位桩（PLAN/01 §11 示例自相矛盾）、类常量表达式全拒、trait 常量不导入、子类静态属性重声明失效、数组/字符串 callable ❌；P3：override 签名/私有 ctor 不校验、接口方法体被接受、ValueError 缺失。测试盲区：现有套件只测同帧 catch、无 typed 属性/析构测试。

## 2026-10-15 OOP 审计修复（P0+P1 全闭环，116/116）
- P0-1 跨帧异常：S_TRY 三处 end_pc=end+1（缩进不一，replace_all 曾漏一处）
- P0-2 __destruct：**op_new_user rc++ 未抵消是总根因**（每对象恒漏一引用）；析构=引擎引用+DTORED+复活判断(rc>1)+GC defer 两段式+收尾快照遍历；GC 补 OBJ 属性边与全 kind 释放；sort/rsort/array_unshift 曾是空操作（写回 args 副本）
- P1-1 类型声明：parse_type_decl 归一化 "?int"/"a\b\c"；tphp_type_accepts（strict+int→float宽化+self/static/parent/类名/closure 伪类型）；五个检查点（recv/recv_variadic/return/assign_prop/assign_sprop）；`C::$x=5` 曾是 Parse error（is_lvalue 过窄）
- P1-2 用户异常类：双世界名字匹配（TExObj 快路 + wrap 字段包装用户 TObj）；exclass.c 注册 16+1 内置异常 TClass（master 表，worker 靠克隆）；**enum from(非法值) 曾静默返回 null**（op-1-n_args 反查 INIT 公式在带参时指错→改存 rec->magic_name）
- P1-3 Closure：闭包 $this 自动捕获（uses 双侧同序+指针去重）；TClosure.scope 重绑（bindTo/bind scope 参、call() 用新对象类）；tphp_vm_call_callable 曾 called_cls=NULL；链式调用 f()() 曾 Parse error
- 环境注意：main.c 等处中文注释经 Edit 写入是合法 UTF-8（clang 输出乱码只是控制台显示）；exclass 走 master 注册后 worker 不可再直接 init（clone 重复 fatal）
- ~~已知待办~~ **进度已全清（2026-10-16 核对）**：TODO-P2-P3.md 已于 a8fb0b2 删除，P2/P3 全部关闭或迁至 **PLAN/01 §10（行为差异权威源）**，基线 **135 passed / 0 failed**，HEAD=fca98d1。后续新缺口一律记 PLAN/01 §10。
- 架构锚点：惰性类常量=TClass.cthunks+compile_thunk(cls)；readonly 写入=TProp.decl 声明类判定；**继承静态属性共享=TProp.decl 共享父槽（重声明才分槽）**；override 签名=class.c 类型格；parent::__construct 帧作用域=VMFrame.vis_scope（与 called_cls 分离）；自工厂=compile_class_decl 壳建后提前注册；try 内声明=pass 系递归 S_TRY（if/循环不递归）；`$obj::$x`=OSPROP 三 handler；测试运行器=cmd /c 引号剥离需 @ 前缀+rc&0xFF

## 2026-10-16 §10 遗留行为差异修复（B1~B8 + C1，基线 147/147）
- **对拍基准=本机 PHP 8.5.1**（双端探针逐字比对后固化 EXPECT）。修掉 10 项：B1 void/never（带值 return=加载期 Fatal 对齐 php `tphp_fatal_src`；never 隐式返回=运行期 TypeError）、B2 动态全局表（global_slot miss→per-thread dyn 条目；顶层出现的名字必有 main 槽故永不分裂）、B3 bindTo "static"=保留原 scope + **全局闭包 $this 也分配捕获槽**（compile_fn_like 与 compile_closure_expr 两处同式重算）+ **private/protected 按声明类可见全链**（TMethod.decl 新字段、tphp_vis_ok private 改 `from==owner`、4 处 prop owner=TProp.decl、trait 拷贝 decl=使用类、worker clone 需 methods 数组独立副本+decl 重指向 decl_remap、消息措辞 meth_scope_disp="scope X/Closure"+"global scope"）、B4 shutdown 按类声明序释放 ClsRt->sprop_vals（cls_ord 注册序表；共享 cvals 不释放=worker 共享写+无析构面）、B5 缺失类不 fatal→OP_NEW_USER 运行期解析 + **"Class '%s'" 全部改双引号**（PHP 8.3+，Trait/Interface 同）、B6 vm_unwind 跨帧归因 pc-=1（thunk 不前推 caller pc 故弹 thunk 后不减；pc_back 状态机）、B7 FinCtx 链续链 emit_pend_tail（trampoline 尾：有外层 finally→占位 JMP 记入外层 pend 由其 patch，链尾才 RETURN）、B8 raise_arg_count（exactly/at_least=必填数、消息内行=call rec 行、getLine=定义行）。
- **C1 动态成员全形态**（用户砍掉 C2 eval——"不安全不常用"）：机制=**复用现有 op + 操作数 kind 分派**（CONST=编译期名/非 CONST=运行期名读 op_member_name），零新增实例侧 opcode；AST prop/mcall/scall 加 `aname`；lexer `$$v`→L_VAR("$v")（普通表达式仍拒"Variable-variable"）；静态侧 CCONST/ASSIGN_SPROP 动态=t1=name、t2=类名前缀、**kind 挂 ext**（a1 被 name 占用）；SMCALL 动态名必须放 **t1**（a2 被 argc 复用——曾放 t2 被 n_sent 覆盖读错 CV）；顺带补齐既有缺口：compile_assign_op 的 A_STATICGET 分支（`C::$p += 5` 从来就报 Cannot assign）、新 OP_ISSET_CCONST（enum/handler表/names表三同步）支撑 `isset/empty(C::$x)`。
- **教训**：①emit_writebacks 曾把互斥 if/else 链拆成两个 if→is_global 节点发完又掉进 else 兜底发垃圾 ASSIGN_DIM（destruct-gc 回归，255）；②member_name_loc 无条件调用且 name=NULL 会 push NULL 字面量→freeze/运行期 AV（oop-static 0xC0000005）；③`lit_str(NULL)`、`a2 双重用途（kind vs argc）`是本类改动的两大坑。
- §10 已重写：10 项修复闭环注记 + 残留=8 条 A 类设计差异 + eval（用户决策不实现）+ 新增边缘差异"动态静态方法名+命名参数=加载期拒绝"。新增 12 个回归测试（147 passed 0 failed）。

## 2026-10-20 语法批次（HEAD=1ddbb70，172/172）

### 编译警告清零（先行）
- 根因：`tphp_raise`/`tphp_vm_raise`/`vm_raise` 未声明 `noreturn` → 十余处 `-Wreturn-type`/`-Wsometimes-uninitialized` 连锁
- 修法：raise 链全标 noreturn（tphp.h/opcode.h/vm.c/lex.c/parse.c + io/net/worker 本地声明）；`tstr_val`/`tstr_len` 参数改 `const TStr*`；miniz 加 `MINIZ_NO_ZLIB_COMPATIBLE_NAMES`（**不可**定义 MINIZ_NO_INFLATE/DEFLATE_APIS——会连带裁掉 archive API）
- clean rebuild 零警告；此后所有新功能提交前保持零警告

### require/include 全家桶（两阶段）
- 阶段1：`require_once`/`include_once` 关键字；表达式形态 `$v=require 'x'` 返回 1/false；include 失败静默跳过；expand_includes 增 pack zip 回落（source_root 剥离+原样 rel）；修 expand 不进 S_FUNCDECL/类方法/闭包体的缺口
- 阶段2：动态路径 `OP_REQUIRE`（ext bit0=is_require bit1=is_once）；运行期 parse+compile 独立 Program 挂 `vm->inc_progs`（resolve_fn 搜索；**inc_progs 来源不写共享 ic**）；被包含文件顶层 return 作为表达式值；TFunc.file/VMFrame.file 支撑嵌套相对路径；运行期 once/busy 线程局部表
- 关键约束：字面量路径仍编译期内联（性能+freeze 友好）；动态路径才走 OP_REQUIRE；普通 require 可重复展开（防环栈），*_once 才去重

### PHP 8.4 属性钩子（核心完整）
- parse：全形式 `{ get { } set { } }` + 短形式 `get =>`/`set =>`；隐式 `$value`；接口 `{ get; }` 声明；拒绝 static/readonly+hooks；短形式 `;` 收尾、块后无分号（对齐 php-src hooked_property 文法）
- compile：钩子编译为 TFunc（is_method，$this slot0）存 TProp.hook_get/set；虚拟属性=钩子体无 `$this->prop` 直接引用（递归 AST 扫描）；虚拟+默认值=加载期 Fatal
- VM：`call_hook` 设 (hook_obj,hook_prop) 上下文，钩子内 `$this->prop` 绕过钩子直读写 backing；读写路径钩子派发在可见性检查**之后**；isset 调 get 非 null 为 true；unset 任一钩子=Error；虚拟读/写无对应钩子=Error
- 后续补齐：`__PROPERTY__`（parse 期 g_hook_prop_name 折叠）、`parent::$p::get()/set()`（A_PROPHOOK+OP_PROPHOOK，从 decl_cls->parent 找钩子）、final 钩子覆盖校验（flatten add_prop 时 Fatal）

### 第一梯队小语法
- `(void)` 转换（8.5）：ctypes id=6，求值后丢弃、表达式值 null
- `final` 提升属性（8.5）：parse_params 认 KW_FINAL → AParam.is_final → TProp.is_final
- `clone($o, ['p'=>v])`（8.4）：A_CLONE 改 u.clone{e,overrides}；编译期展开为 clone+ASSIGN_PROP（走钩子/类型检查）；v1 仅数组字面量键

### 跳过决策（用户拍板）
- 魔法常量 `__CLASS__`/`__FUNCTION__`/`__METHOD__`/`__NAMESPACE__`/`__TRAIT__`：使用频率低，可用 `self::class` 等替代，**不做**

### 包管理器（PLAN/pkg-manager.md；**四阶段已实现**，2026-10-21）
- **无外部工具/无 git**（用户拍板）：包源 = GitHub 仓库 URL（zipball；无 ref → 匿名 API 解析最新发布**固化进 mod.json**）/ HTTP(S) zip 直链 / 本地路径或 zip；纯 C：http_get + miniz 解压
- `tphp new/add/remove/install/upload` + builtin `pkg_new/add/remove/install`；mod.json + mod.lock（`{url, ref, resolved}`，无 commit）
- 包 ID `foo/bar` → `vendor/foo/bar/`；全项目共享拍平；禁止嵌套 vendor；覆盖前 fail-closed 检查
- autoload：PSR-4（长前缀优先，substr 匹配）+ files；install/add/remove 生成 vendor/autoload.php；**无 main 字段、无单一 namespace 字段**
- 打包衔接：build 自动 --with vendor/（strict 跳 tests/*.md）；`.php` 进 zip；pack.include/exclude 解析保留、build 消费后置
- 用户文档 doc/package-manager.md；CLI 入口 doc/cli.md；测试 tests/pkg/ 7 个

### 环境/操作备忘
- Edit 工具改 C 文件中文注释合法 UTF-8；clang 控制台输出乱码只是显示
- PowerShell 5.1 `Set-Content -Encoding UTF8` 写 BOM——测试 fixture 注意
- git 提交时 `build/cacert.pem` 是刻意入库的（.gitignore `!` 豁免），勿误删

## 2026-10-22 FFI 回调 v1 已完成（184/184，PLAN/02 §4.5.1）
- 实现：cdef 参数位函数指针真解析（FFI_FUNC_PTR）+ libffi trampoline（FFIClosure+TLS 表脚本/线程保活，无 close）+ cb_decode/encode（char* 拷贝 string、struct 别名失效、返回不匹配=警告+零值）；test-ffi-callback.php qsort 端到端
- **异常硬禁收口**：`vm_unwind_jump`（src/vm.c）统一所有 `longjmp(vm->x.unwind)`——op_throw 曾直接 longjmp 绕过 depth 检查；tphp_ffi_cb_depth>0 → Fatal "Throwing from FFI callbacks is not allowed"
- 坑：tphp 无 by-ref 捕获；void* 下标须 cast("int*")（PHP 同款）；cdef 函数指针原是占位 void*（"trampoline 留待后续"注释已删）

## 2026-10-22 FFI 回调四家对照与 v1 设计定稿（PLAN/02 §4.5.1）
- 四家对照（PHP/txiki/Bun/V）定稿：API 隐式对齐 PHP、TLS 表脚本期保活、编组修 PHP 返回值垃圾坑、**回调内 throw=Fatal**（raise 钩子查 tphp_ffi_cb_depth——longjmp 穿 trampoline 会毁 C 栈，PHP bailout 同病所以才禁）、同线程允许重入、不做 threadsafe、trampoline 不暴露裸地址
- 实现落点：ext/ffi/ffi.c（parser 函数指针类型 + FFIClosure + trampoline_cb + TLS 表 + encode 分支）+ vm raise 路径 depth 检查 + tests/ffi/test-ffi-callback.php（qsort）
- v2 后置（下次候选，规格见 PLAN/02 §4.5.1）：①struct 字段/数组元素闭包赋值（cdata 写路径识别 FFI_FUNC_PTR）②同闭包 trampoline 去重 ③延迟异常传播（catchable 版，改 unwind 保护帧——复杂度高，与硬 Fatal 是语义取舍）
- 其他待办余项：doc/ffi.md 信任声明（装包即信任一句话）、E3 可选（zip 源哈希/树哈希/verify）、pack.include/exclude 的 build 消费

## 2026-10-21 FFI 安全分析 + lock integrity 设计（docs 已提交待开工）
- **doc/ffi-security.md**：PHP 8.5.7/txiki/Bun 三方对照（只有 PHP 做安全模型=ffi.enable 入口门控+preload；txiki/Bun/tphp=本地可信全开放）。用户拍板：❌ffi.enable 总闸（php.ini 大爆炸）、❌动态库标识/签名（可复制/密钥+系统库豁免被 system() 一行绕穿）；✅信任=声明+包管理层边界
- **lock integrity（PLAN/pkg-manager.md 阶段 E，待实现）**：lock 逐文件 sha1 快照（字段 `integrity`，全文件含 php、明细非树、SHA-1 复用 pack.c static→导出、lock_save 单点写入）；校验点仅 install skip（pkg.c:1944 现只看 emj 存在→升级）与 build 打包；运行时不查；fail-closed 报文件 + `--force`；新增文件算不匹配；旧 lock 兼容自动补写；测试 test-pkg-integrity.php
- 小项待做：`FFI::cast` 拒裸 int（getAddress 保留单向阀）——比三方都严的点
- ~~下次开工顺序建议：阶段 E1 → E2 → cast 收紧~~ **均已完成**（E 8da368a / cast 69bb2ed / 回调 v1 本会话）；当前候选：FFI 回调 v2（见上）或 M7 方向（preg_*/PDO/HTTP server/REPL——均需先立规格）

## 2026-10-20 会话二：包管理阶段 A（174/174，HEAD=8fcee3f 未提交）
- **决策变更（用户拍板）：包管理不依赖 git 应用**——来源=HTTP(S) zip 直链 + 本地路径/zip（http_get+miniz）；ref 精确定位（无 ls-remote，不做 ^~ /latest）；deps=`{url, ref}`（读兼容旧 git 键）。~~PLAN 待阶段 D 改写~~ **10-21 已完成**（PLAN 主体/索引/cli/stdlib/package-manager 边界表全部同步无 git 版）
- 阶段 A 完成：src/pkg.{c,h}（jsonc 剥离→json.c C API `tphp_json_decode_c`→TMod；libuv 同步 fs；tmod pretty writer；autoload 生成=根+vendor 扫描+PSR-4 长前缀排序+首版 stub；`tphp new`；builtin `pkg_new` 测试缝）；tests/pkg/ 2 个；run.ps1 目录表 +"pkg"；零警告
- 附带运行时修复（锚点）：①class_resolve_lc2(lc,orig)——spl_autoload 收原始大小写类名（PSR-4 必需，全调用点已换）②TFunc.prog 反查——动态 require 的独立 Program funcs 表，CLOSURE_INIT 用 vm->prog 会越界 0xC0000005 ③嵌套循环两套语义：默认 run_frame_until_pop 保持旧"帧切换即退出"（改深=析构/magic 时序崩、大面积 0xC0000005）；仅 autoload 用 run_frame_until_pop_deep，且**必须先缓存 stop=f->prev（f 弹出即 free，条件读 f->prev 是 UAF）**
- 杂项坑：stdlib 无 strncmp（生成代码用 substr）；build 每次重生成 bundle_data.c——stash/checkout 撞它先 restore；PS 把 `stash@{0}` 的花括号剥掉
- ~~下次：阶段 B → C → D~~ **已完成（2026-10-21，HEAD 见 1b57f0c 之后的 pkg 提交）**：add/remove/install/upload + 失败诊断测试 + path_join3 修复 + Windows Root 信任库 + GitHub URL v2 语义 + 覆盖 fail-closed；基线 179/179
