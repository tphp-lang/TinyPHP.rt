# OOP 审计修复交接 — P2 / P3 待办（2026-10-15）

> 上阶段（P0+P1）已全部闭环：**116 passed / 0 failed**。
> 跑法：`powershell -NoProfile -ExecutionPolicy Bypass -File tests\run.ps1`
> 新增回归测试 7 个（tests/lang/）：test-exception-crossframe / test-destruct /
> test-destruct-gc / test-array-inplace / test-typed-decls / test-user-exceptions / test-closure。
> 详细执行记录见 `.workbuddy/memory/2026-10-15.md`。
> 每修一项：先跑 `php xx.php` 与 `build\tphp.exe xx.php` 对拍 → 实现 → 全量回归 → 补 `// EXPECT:` 测试。

## P2-A：回收 "lands in M3" 占位桩（原任务 #22）

桩位精确坐标（2026-10-15 实测）：

| 桩 | 位置 | 说明 |
|---|---|---|
| nullsafe `?->` | parse.c:667（P_NULLSAFE） | ⚠️ 该 case **没有 break**，直接落穿到 P_DOUBLE_COLON 分支——改时注意；语义：`$a?->b` 若 $a 为 null 整链短路为 null，可在 compile 层发 JMP_NULL 类序列 |
| new 命名/展开参数 | compile.c:1325 | 参照普通函数调用的命名参数重排逻辑（compile_call has_named 路径） |
| 方法命名/展开参数 | compile.c:1393（INIT_MCALL） | 同上，实参重排后 SEND |
| 静态命名/展开参数 | compile.c:1433（INIT_SMCALL） | 同上 |
| 方法 first-class callable | compile.c:1354（A_FIRSTCLASS_DYN + A_PROP） | `$obj->m(...)` → 编译成闭包：捕获 $obj + 方法名，运行期 INIT_MCALL；注意 `$obj->m(...)` 解析已走 parse_postfix 的 FCC 分支（is_fcc），只是 A_PROP 被拒 |
| return inside catch + finally | compile.c:1772（加载期拒绝） | 需要 pend trampoline 在 catch 体也生效（现在只在 try 体有）；低频，可放最后 |

验收：`$a?->b`、`new P(x: 1)`、`$obj->m(x: 1)`、`S::m(x: 1)`、`$obj->m(...)` 与 PHP 对拍。

## P2-B：语义缺口（原任务 #23）

| 缺口 | 现状 | 提示 |
|---|---|---|
| 类常量表达式 | compile.c CM_CONST（~2579）只接受 eval_const2 纯常量，否则 "Class constant expressions must be constant" | PHP 允许字面量拼接/算术/`self::X`/数组；可复用 compile_thunk（def_thunk 机制已在 TProp/TFunc 落地）+ 首次取用求值 |
| trait 常量不导入 | bind_traits（class.c ~400）只搬 methods/props/sprops，**没搬 cnames/cvals** | 照 add_prop 模式加常量合并（同名 own 优先，冲突 fatal） |
| 子类静态属性重声明失效 | class.c add_sprop（~357）：同名"keep parent slot"，子类声明值与独立存储全丢 → `class D extends A { public static int $k = 5; }` 读到父类值 | PHP：重声明 = 子类自己的存储槽；与 P1-1 的 typed sprop 有交互 |
| 数组/字符串 callable | op_init_dyn（vm.c ~857）只支持 closure/`__invoke`/字符串函数名；`[$obj,'m']()` / `'Cls::m'()` / `'strlen'` 已支持最后者 | op_init_dyn 加 T_ARR（[obj,method]/[cls,method] 两形态）+ T_STR 里含 `::` 的静态调用分发 |
| `$obj::$x` 实例访问静态属性 | parse.c:677 "Only '::class' may follow an expression" | 运行期按对象类解析；低频 |

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

## 环境注意（本会话实测）

- exclass.c 用 `tphp_class_register_compile`（master 表）后，**worker.c 不可再调 tphp_exclass_init**（clone_thread 重复注册会 fatal）；Closure 类走线程表（同 Coroutine 模式）无此问题
- build.cmd 通配 src/*.c，新增 .c 文件自动纳入；中文注释写进 .c 是合法 UTF-8（clang 输出里的乱码只是控制台显示问题）
- 测试 EXPECT 行与实际输出不一致时，先用 `php xx.php` 对拍——本阶段多次失败都是 EXPECT 猜错而非实现错
- 同文件多处编辑注意缩进差异（P0-1 replace_all 漏一处 12 空格版）；编辑后 grep 复核关键行
