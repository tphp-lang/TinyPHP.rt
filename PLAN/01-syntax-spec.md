# 01 · 语法兼容规格（兼容到什么程度）

> 上级索引：[../PLAN.md](../PLAN.md)。**总基线：对齐 PHP 8.x 的用户可见语义。** 抛掉的是实现层包袱（引用模型、错误双轨、动态变量），不是用户可见语法。本项目是解释型运行时，动态特性全部保留。
>
> **术语**：本文档中的"编译/加载期"指 **源码 → opcode 的加载阶段**（lex → parse → AST → 字节码，任何脚本执行前必经，与 php-src 的 `zend_compile.c` 同义），"编译期报错" = **加载期报错**（脚本未执行第一行即被拒绝，对应 PHP 的 `ParseError/CompileError`）；"运行期"指 VM 解释执行 opcode 的阶段。与 AOT/机器码无关。

## 1. 词法与文件结构

| 特性 | 支持情况 |
|---|---|
| `<?php` 开标签 / `?>` 闭标签 | ✅ 完整支持（含闭标签后吞掉单个换行，对齐 PHP） |
| 内联 HTML 混写 | ✅ 支持 |
| `<?=` 短输出 | ✅ 编译为 print 调用 |
| 注释 `//`、`#`、`/* */`、doc 注释 | ✅（doc 注释仅保留文本，M7 起可用于 API 文档） |
| 标识符 | 对齐 PHP（`$` 前缀；支持 UTF-8 标识符） |
| 关键字 | PHP 8.5 全集（大小写不敏感规则对齐） |
| 数字字面量 | 十进制/十六 `0x`/八 `0o`/二 `0b`、下划线分隔 `1_000_000`、浮点 `1.5e3`、`INF/NAN` |
| 单引号字符串 | ✅（仅 `\'` `\\` 转义，不插值） |
| 双引号插值 | ✅ `$var`、`{$expr}`（含 `{$obj->prop}`、`{$arr['k']}`、`{$obj->m()}`） |
| `${var}` 花括号偏移插值 | ❌ 对齐 PHP 8.2 deprecation → 直接编译错误 |
| heredoc / nowdoc | ✅ M2 支持（含缩进剥离，对齐 PHP 7.3） |

## 2. 类型系统

| 类型 | 支持 | 说明 |
|---|---|---|
| null / bool / int(int64) / float(f64) / string | ✅ | 与 PHP 一致（int 溢出自动转 float 对齐） |
| array | ✅ | 有序哈希表，list/dict 双语义（灵魂特性） |
| object / closure / enum | ✅ | |
| 资源 resource | ❌ | 新运行时无此概念；I/O 句柄封装为对象（如 `File`、`Socket`） |
| 类型声明 | ✅ | 参数/返回值/属性：`int` `?int`（可空）`int\|string`（union）`array` `mixed` `iterable` `static` `self\|parent`、类名、enum 名 |
| 交叉类型 / DNF | ✅ | C 批已实现（2026-10-23）：`A&B`、`(A&B)\|null`（DNF，括号 parse 期消费不入库）；交叉成员仅 class-like + object/self/parent/static；union 内交叉必须括号、`?` 不能修饰交叉（对齐 PHP 8.2）；见 §12.1 |
| 联合类型运行期校验 | ✅ | 运行期强制；加载期能静态推断的直接报错 |
| int/float 自动宽化 | ✅ | int→float 传参允许（对齐 PHP）；float→int 非法 |

**注意**：类型声明是**运行期校验**（解释器行为），不是 AOT 约束——动态传入 `new $cls` 得到的对象、`$obj->$m()` 调用完全合法，校验发生在调用点。

## 3. 语句与控制流（全支持）

`if/elseif/else`、`while`、`do-while`、`for`、`foreach`（值/键值；**foreach 引用 `&$v` 不支持**，见 §6）、`switch`（松散比较语义对齐 PHP）、`match`（严格比较）、`break N / continue N`、`return`、`goto`（便宜，做；进出 try/catch 块除外）、`declare(strict_types=1)`（冗余但可写）、`require/include/require_once/include_once`（字面量编译期内联；**动态路径 OP_REQUIRE 运行期**；`*_once` 去重；表达式返回 1/false/文件 return 值）、`global`（读写均支持：编译期别名，函数内读写直接编译为全局表访问，无需引用）、`static` 局部静态变量、`unset()`、`echo/print`。

## 4. 表达式与运算符

- **优先级与结合性完全对齐 PHP 8**（含 `and/or/xor` 优先级低于 `=`、`??` 低于 `?:`、`<=>`）；`.` 与 `+/-` 混用不加大括号 → 编译错误（对齐 PHP 8 deprecation 并收紧为报错）
- 全套：算术 `+ - * / % **`、位 `& | ^ ~ << >>`、拼接 `.` `.=`、复合赋值 `+= -= *= /= %= **= ??= &= |= ^= <<= >>= .=`、递增递减 `++ --`（前/后，null 递增对齐 PHP）、比较 `== != === !== < <= > >= <=>`、逻辑 `&& || ! and or xor`、三元 `? :` 与 `?:`、null 合并 `??`、nullsafe `?->`、错误控制 `@`（**禁用**）、类型转换 `(int)(float)(string)(bool)(array)`、`instanceof`、`clone`、管道 `|>`（PHP 8.5：`$x |> f($y, 2)` ≡ `f($x, $y, 2)`，编译期展开为函数调用）、引用传值 `&`（**禁用**）
- 数组：`[]` 与 `array()`、键值混用、自动追加 `$a[] = x`、展开 `[...$a, ...$b]`（含字符串键合并语义对齐）、解构 `[$a, $b] = ...`、`['k' => $v] = ...`、`list()` 旧语法 ✅（与 `[]` 解构等价，编译为同一套 opcode）
- **动态特性（解释器基本能力，全部保留）**：`$fn()`、`new $cls`、`$obj->$method()`、`$cls::$method()`、`$obj->{'prop'}`、可变类常量 `$cls::CONST`、first-class callable `$obj->m(...)`

## 5. 函数与闭包

- 顶层函数 + hoisting（先调用后声明，对齐 PHP）；**嵌套条件函数声明 ❌**（简化）
- 默认参数（含常量表达式）、可变参数 `...$args`、命名参数 `f(b: 2)`、展开传参 `f(...$arr)`
- 类型声明全套（见 §2）；引用传参 ❌
- 闭包：`function() use ($a)` 仅值捕获（**`&$b` 引用捕获 ❌**）、箭头函数 `fn() =>`（自动按值捕获）、`static function/fn`、`Closure::bind/call`、`$f = $obj->method(...)`
- 变量函数与函数别名 `use function`

## 6. OOP（完整版，M3）

| 特性 | 支持 |
|---|---|
| class / abstract / final / 匿名类 | ✅ |
| extends / implements / 接口常量 / 接口默认方法 | ✅ |
| trait：use 多个、`as` 重命名改可见性、`insteadof` 冲突解决、trait 内抽象方法；绑定顺序对齐 PHP 8.5（trait 先于 parent） | ✅ |
| enum：纯 case + backed enum(int/string) + case 方法 + implements + `->value` + `::from()/tryFrom()` | ✅ |
| 可见性 public/protected/private（属性/方法/常量）、static、readonly 属性 | ✅ |
| 构造器属性提升 `public function __construct(private string $name)` | ✅ M3 |
| 魔术方法 | ✅ `__construct __destruct __get __set __isset __unset __call __callStatic __toString __invoke __clone __debugInfo __cloneState(跨线程克隆与 serialize 共用协议，见 PLAN/02 §3)` |
| 静态与晚期绑定 self / static / parent / $this / `static::class` | ✅ |
| 对象比较 `==`（属性递归宽松比较）/ `===`（同实例） | ✅ 对齐 PHP |
| 属性钩子 property hooks（PHP 8.4） | ✅ `get`/`set` 全/短形式、隐式 `$value`、虚拟属性判定、isset 调 get、unset 禁止、readonly 拒绝、接口 `{ get; }` 声明、`__PROPERTY__`、`parent::$p::get()/set()`、`final` 覆盖校验；`&get` 引用返回后置 |
| 弱引用 WeakMap/WeakReference | ❌ 后置 |

## 7. 异常体系（简化但形态一致）

```
Throwable
├── Error（引擎错误基类）
│   ├── TypeError / ArgumentCountError / DivisionByZeroError
│   ├── UndefinedError（替代 PHP 的 warning+null：读未定义变量/属性/键）
│   └── CompileError
└── Exception（用户异常基类）
    ├── RuntimeException / LogicException / InvalidArgumentException
    ├── ErrorException（无错误双轨，仅在用户显式需要时用）
    └── AsyncCancelled（协程取消，直接继承 Throwable，防 catch(Exception) 误吞）
```

- `try/catch(多类型 | 合并)/finally`、异常链 `previous`、堆栈打印（文件:行:函数，对齐 PHP 格式）
- **无 warning/notice 层级**：一切引擎诊断都是异常；`set_error_handler / error_reporting / trigger_error` ❌

## 8. 命名空间

`namespace A\B;`、`use A\B\C; use A\B\C as D; use function A\max; use const A\PI;`、限定名解析规则完全对齐 PHP（相对/绝对/回退）。`spl_autoload_register` ✅（类名未命中时调用注册回调再重查，带递归上限；未注册则抛类未找到错误）。

## 9. 明确不支持清单（加载期报错——脚本执行前即被拒绝，给出替代建议）

| 不支持 | 原因 / 替代 |
|---|---|
| `&$var` 引用全部形式（传参/赋值/foreach/返回/&new） | 引用与协程挂起/跨线程克隆/简单 GC 冲突（**非 AOT 原因**）；用对象（天然共享）或返回值 |
| `$$var`、`${'name'}`、`extract()` | 破坏静态分析（按名字**写**变量需动态作用域机器，VM 局部变量从静态槽位退化为哈希表）；用数组显式写法 |
| `@` 抑制符 | 错误双轨已废除；用 try/catch |
| `eval` / `create_function` | 安全 + 单二进制无意义 + 需动态作用域（访问调用方局部变量，同 `extract()` 的机器） |
| `each()` | PHP 8 已删除，对齐不支持；用 foreach（`list()` 语法本身已支持，见 §4） |
| `${var}` 插值 | 对齐 PHP 8.2 deprecation；用 `{$var}` |
| `set_error_handler` / `error_reporting` / `trigger_error` | 错误统一为异常 |
| 生成器 `yield / yield from` | 规划后置 M7+（协程已覆盖主要场景，届时可用协程实现）——路线见 §12.2-A |
| 注解 `#[Attr]` | 语义后置 M7+（路线见 §12.2-B）；**词法器必须特判 `#[`**（否则被静默吞掉、无任何报错），判据照抄 php-src `zend_language_scanner.l:1390`：`#` 后第一个字符**不是** `[` 才是行注释；`#[` 识别为属性记号后在加载期明确报错 |
| `declare(ticks/encoding)`、`set_time_limit` | 无意义 |
| `goto` 进出 try/catch 块 | 其余 goto 允许 |

## 10. 与 PHP 的行为差异（有意为之，全部列出）

> **2026-10-16 修复闭环**：历史清单中的 10 项行为缺陷/缺口已全部修复并与
> PHP 8.5.1 逐条对拍——`void`/`never` 返回校验、`global` 槽缺口、`bindTo
> "static"`（含 private/protected 按声明类可见的全链语义）、shutdown 静态
> 属性析构序、缺失类加载期报错、try 归因 quirk、多层嵌套 finally 的
> return、ArgumentCountError 消息尾巴、`$obj->$name` 动态成员全形态
> （读/写/调用/nullsafe/fcc/链式/isset/unset/静态 `C::$$v` 与 `C::$v()`），
> 以及顺带补齐的既有缺口 `C::$x op= v`、`isset/empty(C::$x)`。回归测试见
> `tests/lang/test-{void,never}-*`、`test-argcount-message`、
> `test-global-closure-method`、`test-finally-nested`、
> `test-exception-next-try`、`test-missing-class`、`test-dynamic-member`、
> `tests/oop/test-{bindto-scope,static-dtor-order,isset-empty-static}`。
> 下列为仍存在的**有意差异**与**唯一未实现项**。

| 场景 | PHP 8 | TinyPHP.rt |
|---|---|---|
| 读未定义变量 | Warning + null | 抛 `UndefinedError`（加载期可静态检测的先报错） |
| strict_types | 需显式声明 | 全局隐含 strict（declare 可写但冗余；仍是运行期校验，非加载期擦除） |
| warning/notice 诊断 | 不中断 | 全部升级为异常（含动态成员名不可见/不存在、数组等非字符串成员名的字符串化） |
| 解构非数组值（8.5 新增 warning，UPGRADING 实锤） | 提示后继续 | 抛 `TypeError`（"诊断即异常"既定规则；**NULL 例外**：`[$a] = null` 在 PHP 8.5 合法无警告、产出全 null，对齐放行） |
| float→int 越界 cast（8.5 新增 warning） | 提示后继续 | 抛 `TypeError`（转换越界；8.5 同时覆盖显式 `(int)` 与隐式两种） |
| NAN cast 到其他类型（8.5 新增 warning） | 提示后继续 | 抛 `TypeError` |
| 无 `declare` 开销差异 | — | `<?php` 外的内联 HTML 允许 |
| `asort([])` 等排序函数传字面量 | Error "could not be passed by reference" | 无引用机制：字面量/临时数组被静默排序后丢弃（需变量才有观察效果） |
| 未初始化 typed 属性读取 | Error "must not be accessed before initialization" | 抛 `UndefinedError`（同为致命，措辞不同） |
| 动态静态方法名 + 命名参数 `C::$v(a: 1)` | 运行期按真实签名重排 | 加载期拒绝（命名参数重排需编译期签名） |
| `eval()` | 支持 | **不实现**（决策：注入面不安全、实际使用率低）；调用报 `Call to undefined function eval()` |
| `preg_match` 的 `$matches` 引用输出 | `&$matches` 自动创建 | **无引用**：要求已初始化数组（`$m = []; preg_match($p, $s, $m);`）原地清空填充（`sort` 同哲学）；未定义/非数组 → TypeError（引导消息） |
| 属性钩子 `&get` 引用返回 | 支持 | v1 按普通 `get` 执行（引用返回后置） |
| 属性钩子 `parent::$p::get()` | 支持 | ✅ 已实现（`A_PROPHOOK` + `OP_PROPHOOK`） |
| 属性钩子 `final` override 校验 | 编译期拒绝子类覆盖 final 钩子 | ✅ 已实现（flatten 时校验） |
| 属性钩子 `__PROPERTY__` 魔术常量 | 支持 | ✅ 已实现（parse 期折为属性名字符串） |
| `(void)` 转换（PHP 8.5） | 支持 | ✅ 求值后丢弃，表达式值 null |
| `final` 提升属性（PHP 8.5） | 支持 | ✅ `__construct(final private int $x)` |
| `clone` 属性列表（PHP 8.4） | 支持 | ✅ `clone($o, ['p'=>v])`（字面量数组；走 ASSIGN_PROP） |

其余语义（数组键、`==`、字符串算术 "123"+1=124、整除零抛 DivisionByZeroError、非数字字符串算术抛 TypeError 等）**全部对齐 PHP 8**。

## 11. 语法能力验收示例（M3 结束应全部可跑）

```php
<?php
// strict 类型全局默认（declare 可省略）
const MAX_LEVEL = 100;

function fib(int $n): int {
    return $n < 2 ? $n : fib($n - 1) + fib($n - 2);
}

// 数组：list/dict 双语义，键规范化对齐 PHP
$list = [3, 1, 2];
$map  = ['a' => 1, 'b' => 2, 99];     // 自动 int 键 0 => 99
$map[] = 'append';                     // 追加键 1
$map['8'] = 'int-key';                 // 键为 int 8（对齐 PHP）
sort($list);                           // [1,2,3]

// 插值与拼接
echo "fib(10) = " . fib(10) . "\n";
echo "first={$list[0]}, b={$map['b']}\n";

// foreach / match / ?? / nullsafe / 解构 / 展开
foreach ($map as $k => $v) {
    $label = match(true) { $v < 2 => 'small', default => 'big' };
    echo "$k => $v ($label)\n";
}
[$x, $y] = [1, 2];
$merged = [...$list, 4];               // [1,2,3,4]
$obj = null;
echo $obj?->name ?? 'anonymous', "\n";

// 闭包与 first-class callable
$add = fn(int $a, int $b): int => $a + $b;
echo $add(1, 2), "\n";
$adder = $add(...);                    // first-class callable
echo $adder(3, 4), "\n";

// 类型声明 + union + 可空
function parse(string|int|null $raw): ?int {
    return $raw === null ? null : (int)$raw;
}

// 动态特性（解释器天然支持）
$fnName = 'fib';
echo $fnName(5), "\n";                 // 变量函数
class Math2 { public static function half($x) { return $x / 2; } }
$cls = 'Math2';
echo $cls::half(10), "\n";             // 动态静态调用

// goto 与 static 局部变量（对齐 PHP）
function counter(): int {
    static $n = 0;
    return ++$n;
}
```

```php
<?php
// ---------- OOP 完整版示例（M3 验收代码） ----------
namespace App\Model;

interface Speakable {
    const VERSION = 1;
    public function speak(): string;
}

trait Greet {
    public function hello(): string {
        return "Hello, {$this->speak()}";
    }
}

enum Status: string implements Speakable {
    case Active = 'A';
    case Closed = 'C';

    public function speak(): string {
        return "status={$this->value}";
    }
    public static function fromLabel(string $s): static {
        return match($s) { 'on' => self::Active, default => self::Closed };
    }
}

abstract class Animal implements Speakable {
    use Greet;

    public function __construct(protected readonly string $name) {}

    abstract public function speak(): string;

    public static function describe(Animal $a): string {
        return static::class . " [{$a->name}] says " . $a->speak();
    }
}

final class Dog extends Animal {
    public function speak(): string { return 'woof'; }
}

$d = new Dog('Rex');
echo Animal::describe($d), "\n";            // Dog [Rex] says woof
echo $d->hello(), "\n";                     // Hello, woof
echo Status::fromLabel('on')->speak(), "\n";// status=A
echo Status::Active::class, "\n";           // App\Model\Status
```

## 12. 语法补全路线（2026-10-23 对照 php-src 8.5.7 分析定稿）

> 现状：§9"明确不支持清单"中大部分是**有意决策**（`&$ref`/`extract`/`eval`/`@`/错误处理器——与协程/单二进制/静态分析模型冲突，不翻案）。
> 真正的语法缺口收敛为下述三档；近期批次 = C 小语法包，A/B 为 M7 立项候选。

### 12.1 近期批次：C 小语法包（编译器/类型层增量，风险低）✅ 已完成（2026-10-23）

| 项 | PHP | 落点 | 状态 |
|---|---|---|---|
| **C1 交叉类型 `A&B` + DNF `(A&B)\|C`** | 8.2 | parse_type_decl 重写（段抽 parse_type_one_seg；括号组消费不入库；union 内裸交叉/`?`修饰交叉拒绝；class-like 白名单）+ tphp_type_accepts 段内 `&` 合取（type_seg_chain_ok；type_seg_ok 类名分支改按 len 拷贝——曾 NUL 扫描吞整串） | ✅ |
| **C2 typed class constants** `const int X = 1;` | 8.3 | parse.c 类 const 加类型前缀（L_IDENT+前瞻=` 判别类型/名字）；TClass.ctypes 并行数组（trait/接口合并 + worker clone 同步）；FETCH_CCONST 求值后校验（thunk 结果同样过检），违约 TypeError | ✅ |
| **C3 new in initializers** | 8.1 | 参数默认值/属性默认值/类常量/promoted 四处均通——修 VM bug：**thunk 嵌套循环改 deep 语义**（旧"帧切换即退出"让 ctor 帧成孤儿 $this=null；run_frame_until_pop_deep 同 autoload 修复，stop=f->prev 先缓存防 UAF） | ✅ |

批次验收：`tests/lang/test-{types-intersection,typed-const,new-in-initializers}.php`；lang 目录 68 全绿。

### 12.2 M7 立项候选（单独立项，先立规格再动手）

| 项 | PHP | 说明 |
|---|---|---|
| **A 生成器 `yield` / `yield from`** | 5.5 | 唯一影响写法习惯的大特性（框架生态普及）。实现 = 编译器函数内状态机（yield 点暂停/恢复，**不依赖调度器**——与协程是两套机器）+ `Generator` 内部类（current/key/next/send/throw/getReturn/valid）+ `yield from` 委托转发。预估数天；立项时先出 PLAN 规格（opcode 集、状态保存、与异常/finally 交互） |
| **B 注解 `#[Attr]` + 反射读取** | 8.0 | lexer 现特判拒绝（`Attributes not supported`）。正经做 = 属性记号 → AST → 存储（类/方法/属性/参数/常量）→ `ReflectionClass::getAttributes` 最小集 + `Attribute` 类标记 + 实例化校验。tphp 生态早期、无框架消费反射元数据——**等需求出现再立项**，避免做了空转 |

### 12.3 不做（记录防翻案）

- 属性钩子 `&get` 引用返回——与 `&$ref` 模型冲突（§6 已注）
- `${var}` 插值、`each()`——对齐 PHP 自身废弃
- `declare(ticks/encoding)`——无意义
