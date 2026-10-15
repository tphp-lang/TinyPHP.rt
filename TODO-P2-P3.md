# OOP 审计修复交接 — P2 / P3 待办（2026-10-15）

> 上阶段（P0+P1）已全部闭环：**116 passed / 0 failed**。
> 跑法：`powershell -NoProfile -ExecutionPolicy Bypass -File tests\run.ps1`
> 新增回归测试 7 个（tests/lang/）：test-exception-crossframe / test-destruct /
> test-destruct-gc / test-array-inplace / test-typed-decls / test-user-exceptions / test-closure。
> 详细执行记录见 `.workbuddy/memory/2026-10-15.md`。
> 每修一项：先跑 `php xx.php` 与 `build\tphp.exe xx.php` 对拍 → 实现 → 全量回归 → 补 `// EXPECT:` 测试。

## P2-A：回收 "lands in M3" 占位桩（原任务 #22）

桩位精确坐标（2026-10-15 实测；**nullsafe `?->` 已完成**——2026-10-15 提交 c7aad8d，lex.c 顺带修了 `?->` 只吃 2 字符的词法 bug；剩余桩位行号因插入有偏移，以搜索为准）：

| 桩 | 位置 | 说明 |
|---|---|---|
| ~~nullsafe `?->`~~ | ~~parse.c:667~~ | ✅ 完成（c7aad8d；AExpr.nullsafe + 每级 JMP_NULL 守卫 + 链短路；顺带修 lex `?->` 只吃 2 字符） |
| ~~方法命名参数~~ | ~~compile.c:1393~~ | ⚠️ **架构限制保留 fatal**：实例方法 receiver 类型编译期未知，重排需运行期 named 机制（新 opcode + RECV 按名匹配，P4 级）；错误消息已指引替代写法 |
| ~~静态命名参数~~ | ~~compile.c:1433~~ | ✅ 完成（c45c8b7；TMethod.param_names + ensure_flat/find_method 签名解析，继承/trait 自动；kind=2 static:: 用 decl_cls 签名近似——重写改名场景已知偏差） |
| ~~new 命名参数~~ | ~~compile.c:1325~~ | ✅ 完成（c45c8b7；ctor 签名同机制；a2=n_params 精确故 op_new_user 参数寻址不破） |
| ~~方法 first-class callable~~ | ~~compile.c:1354~~ | ✅ 完成（5157326；OP_FCC_METHOD + GC_FLAG_MFCC 闭包（captures=[receiver,name]），op_init_dyn 分发；call_user_func/array 经 tphp_vm_call_callable 方法 frame。**TODO：FCC 闭包不重查可见性**（绑私有方法可绕过，与 __invoke 分支同现状）；`C::m(...)` 静态版未实现 |
| return inside catch + finally | compile.c:1772（加载期拒绝） | 需要 pend trampoline 在 catch 体也生效（现在只在 try 体有）；低频，可放最后 |

验收：`$a?->b`、`new P(x: 1)`、`$obj->m(x: 1)`、`S::m(x: 1)`、`$obj->m(...)` 与 PHP 对拍。

## P2-B：语义缺口（原任务 #23）

| 缺口 | 现状 | 提示 |
|---|---|---|
| 类常量表达式 | 现状不变 | compile.c CM_CONST（~2655）只接受 eval_const2 纯常量；可复用 compile_thunk + 首次取用求值 |
| ~~trait 常量不导入~~ | ✅ 完成（6e24bdc；bind_traits 导入，同名已有者保留——PHP 8.3 真 collision fatal 近似跳过，P3 对拍收紧） | |
| 子类静态属性重声明失效 | 现状不变 | class.c add_sprop（~358）：同名"keep parent slot" |
| ~~数组/字符串 callable~~ | ✅ 完成（5f1629e；op_init_dyn + tphp_vm_call_callable 双路径：[$obj,'m']/['Cls','m']/'Cls::m'（最后一个 :: 分割）+ native 方法；usort/array_map/call_user_func 全通。**TODO：这些形态不重查可见性**） | |
| `$obj::$x` 实例访问静态属性 | 现状不变 | parse.c:677 |

验收样例全部与 PHP 对拍（尤其 trait 常量冲突语义、子类静态属性独立存储）。

## P3：校验缺失与非标准扩展修补（原任务 #24）

| 项 | 现状 | 提示 |
|---|---|---|
| override 签名校验 | add_method（class.c ~244）只查可见性收缩/final；参数数/类型/返回类型不比对 | flatten 时子方法 vs 父方法签名比对（PHP 8：不兼容 → fatal）；P1-1 的 TFunc.param_types/ret_type 已有数据 |
| 私有 ctor 不阻断 | op_new_user 不查 ctor 可见性 → `new` 外部类私有构造成功 | op_new_user 加 vis 检查（scope=调用方 frame_scope_cls） |
| 接口方法体被接受 | parse_class_member 不分 kind，接口里 `function f() { body }` 照收 | kind==CK_INTERFACE 且有 body → parse error |
| readonly class | CLS_READONLY 标志已解析存储（value.h:124），**无任何执行** | 实例化后属性一律不可写（含 __set 路径）；与 readonly 属性检查同点（op_assign_prop ~1762） |
| asymmetric visibility | `public private(set) $x` 解析报错 | parse_class_member 修饰符循环 + TProp 加 set 可见性；op_assign_prop 用 set 可见性 |
| is_a / is_subclass_of | 内置函数缺失 | builtin.c 照 tphp_class_isa_name 实现 |
| ~~ValueError~~ | **P1-2 已顺带完成**（TExClass 表 + exclass.c + enum from() 已抛 ValueError） | 剩：把其他该抛 ValueError 的引擎点改类型（如数组长度为负等，逐点对拍） |
| 错误措辞 | 部分 M3 时代措辞（"lands in M3" 已清） | 逐条对拍 php-src 消息格式 |

## 已知分歧（文档既定，动前先确认）

1. **strict vs coercive**：PLAN/01 全局 strict——`return 42` 给 `: string` tphp 报 TypeError、PHP 默认能过。若要改 coercive 需重设计 tphp_type_accepts。
2. `$obj::$x`（实例访问静态属性）不支持 → 见 P2-B。
3. `void/never` 返回类型解析接受但不校验。
4. 未初始化 typed 属性读取报 UndefinedError（PHP 措辞 "must not be accessed before initialization"）。
5. `global $x` 若 $x 在顶层从未出现 → "Internal: global slot missing"（编译期全局槽分配缺口；闭包/global 深层修复时一起看）。
6. bindTo 的 $scope 不接受 `"static"` 特殊值（只有真实类名字符串/`X::class`）。
7. GC：纯对象环回收已修（P0-2），但 gc_children 对 **sprop_vals / cvals 不遍历**（静态属性/类常量里的堆值不参与 GC 计数——现状靠 program 生命周期兜底）。


## P2/P3 进展与遗留（2026-10-15 第二段，124/124）

### 本次新完成（未提交，工作区）
- ✅ **类常量表达式全惰性**（对齐 PHP 前向引用/跨类/全局常量）：CM_CONST 不可折叠 → compile_thunk（新增 cls 参数，self::/parent:: 在 thunk 内正确解析）+ TClass.cthunks 平行数组 + 首次取用求值缓存（op_fetch_cconst）+ 环检测（self-referencing fatal；unwind 复位支持 catch-retry）
- ✅ **子类静态属性重声明 = 独立存储槽**（add_sprop：A::$x 与 B::$x 分离，flat 表条目仍替换）
- ✅ **P3 批次**：
  - private/protected ctor 阻断（op_new_user + vis 检查）
  - 接口方法体/属性拒绝（parse_class_member，CK_INTERFACE）
  - readonly class 落地：untyped 实例属性 fatal（CM_PROP + 提升参数两处）+ 写入作用域规则（TProp.decl 声明类判定；只允许声明类作用域写，子类也不行；op_assign_prop 重构）
  - override 签名校验（class.c add_method）：static 翻转 fatal / 参数只增不减（variadic 父豁免）/ 参数类型逆变 / 返回类型协变（类型格：精确相等/mixed/int⊂float/array⊂iterable/类名⊂object/类层级 isa）
  - **parent::__construct 帧作用域修正**：VMFrame.vis_scope 覆盖位（可见性取声明类 Animal，static:: 仍后绑定 DerivedJob）——否则父类 ctor 写父类 readonly 属性被误拒、static:: 日志串类
- ✅ P3 is_a/is_subclass_of/get_parent_class（另一段会话 31ea14c）

### 本段新增完成（第三段续）
- ✅ **`new SameClass()` 自工厂编译 bug 已修**：compile_class_decl 壳建好后提前注册（成员编译期原地填充，与 flatten 惰性填充同模式）；跨类前向 `new`（A 用后声明的 B）仍是缺口
- ✅ **`readonly class` 解析放行**（parse.c 类修饰符 + CLS_READONLY；执行层早已就位，即改即生效）
- ✅ P3 专属测试落地：tests/oop/test-oop-p3.php（13 断言）
- 状态：**125 passed / 0 failed**

### 本段发现的新 bug（未修，下次优先）
1. ~~**`new SameClass()` 在 static 方法内编译期报 "Class not found"**~~ ✅ 已修（f307961 compile_class_decl 提前注册自工厂；2026-10-15 复核 probe ok）
2. ~~**try 块内的类声明被静默丢弃**~~ ✅ 已修（pass_classes/pass_funcs/pass_sigs 增加 S_TRY 递归；**同时收窄**：不进 if/循环/switch——与 PHP 8 条件声明语义一致，if(false){class X} 不定义。测试 tests/oop/test-oop-try-hoist.php）
3. FCC 闭包与数组 callable **不重查可见性**（绑私有方法可绕过）——上一段 TODO 遗留
4. 类常量惰性求值与 freeze 交互未验证：worker 线程克隆后 thunk 求值路径待测

### 遗留任务（原序不变）
- P2-A：方法命名参数（P4：需运行期 named 机制）、return inside catch+finally
- P2-B：`$obj::$x` 实例访问静态属性（parse.c:677）
- P3：错误措辞对拍、ValueError 其余点、接口方法可见性约束
- **补测试**：本段 P3 修复（ctor 可见性/接口体/readonly class/override 签名）暂无专属 EXPECT 测试——探针脚本已验证（/tmp/exprobe/p1-p5, o1-o4, r1-r2），下次先落 tests/oop/test-oop-p3.php

## 环境注意（本会话实测）

- exclass.c 用 `tphp_class_register_compile`（master 表）后，**worker.c 不可再调 tphp_exclass_init**（clone_thread 重复注册会 fatal）；Closure 类走线程表（同 Coroutine 模式）无此问题
- build.cmd 通配 src/*.c，新增 .c 文件自动纳入；中文注释写进 .c 是合法 UTF-8（clang 输出里的乱码只是控制台显示问题）
- 测试 EXPECT 行与实际输出不一致时，先用 `php xx.php` 对拍——本阶段多次失败都是 EXPECT 猜错而非实现错
- 同文件多处编辑注意缩进差异（P0-1 replace_all 漏一处 12 空格版）；编辑后 grep 复核关键行
