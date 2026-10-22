# FFI 安全分析（三方对照）

> 2026-10-21 基于源码阅读的对比分析：PHP 8.5.7（`ext/ffi`）、txiki.js（`src/mod_ffi.c`）、Bun v1.3.14（`src/runtime/ffi/`），对照 TinyPHP.rt 现状。
> 结论先行：**FFI 的不安全是本质，不是实现缺陷**；四方里只有 PHP 做了安全模型，因为它面对的是"互联网上跑别人的代码"。TinyPHP.rt 的定位与 txiki/Bun 相同（本地可信脚本），当前行为合理；唯一需要新决策的变量是**包管理器带来的第三方 vendor 代码**。

## 1. 威胁模型：为什么 FFI 无法"做安全"

FFI 的功能承诺就是"直接调用 C、直接读写原生内存"。以下能力由 API 面直接给出，任何防护都只是在门口检查：

- **任意地址读写**：整数 → 指针转换 + 指针视图解引用
- **任意代码执行**：`dlopen` 任意动态库 + 调用任意导出符号
- **绕过一切 PHP/JS 层限制**：`disable_functions`、`open_basedir`、模块黑名单都只拦语言函数表，拦不住 `ffi_call("system", ...)`

因此 FFI 的安全问题不是"修 bug"，而是**选择威胁模型并划门控边界**：

| 威胁模型 | 代表 | 合理答案 |
|---|---|---|
| 服务端多租户（跑陌生人的代码） | PHP（shared hosting 时代遗产） | 默认锁死入口，运维预授权符号集 |
| 本地可信脚本（跑自己的代码） | txiki.js / Bun / TinyPHP.rt | 信任脚本 = 信任本机代码；安全工程转向生命周期/防崩溃，而非防恶意 |

## 2. PHP 8.5.7：唯一的"认真"安全模型

实现：`ext/ffi/ffi.c`（~8k 行）+ `ffi_parser.c`（llk 生成）+ libffi。

### 2.1 `ffi.enable` 入口门控（唯一防线）

| 档位 | 行为 |
|---|---|
| `true` | 全开 |
| `false` | 全禁 |
| `preload`（**默认**） | 仅 CLI、预编译函数、或 preload 编译期可调 FFI API |

- **检查点在全部 17 个静态方法入口**（`zend_ffi_validate_api_restriction`，cdef:3057 / load:3623 / scope:3647 …）；**CData 读写与 ffi_call 本身不再检查**——拿到对象即畅通。
- **preload/scope = 预授权**：管理员用头文件（`/*lib*/`、`/*scope*/` 指令）在 `ffi.preload` 阶段定义符号集，运行期 `FFI::scope($name)` 只能取用已授权范围，不能现场 `cdef`。本质是把 cdef 权限从请求方收回运维方。
- 源码自留 TODO："this might need to be disabled or protected???"（ffi.c:3084/3438）。

### 2.2 类型安全的真实边界

- 有：固定数组下标边界（"C array index out of bounds"）、函数调用参数个数/类型检查、回调内禁抛异常。
- 无：指针算术（`zend_ffi_add`）、指针下标、`memcpy/memset` 目标、**`cast("int*", addr)` 整数→指针**、struct 字段（仅查 NULL/字段名）。

**PHP 的态度**：不把 FFI 做安全，而是**默认不给你**；给了就是原生扩展权限，官方文档明说。

## 3. txiki.js：零门控 + 生命周期工程

实现：`src/mod_ffi.c`（libffi）+ `src/js/stdlib/ffi/ffi.js`（`dlopen`/`dlopenCProto` 原型解析，无通用 cdef 字符串语言）。

- **零权限门控**：`tjs:ffi` 恒注册；`dlopen` 任意 so、`uv_dlsym` 任意符号。
- **内存零边界**：`Pointer.createPointer(bigint)` 从任意整数伪造指针（注释自认 UB）；`read.u8…` / `toUint8Array` 裸地址零拷贝。文档原话：**"FFI is inherently unsafe"**。
- 唯一"防护"：类型错误（非指针拒绝）、`optional` 符号容错、回调异常在 `ffi_call` 边界置零返回值而非 abort 进程。
- **值得借鉴的生命周期工程**（非安全）：
  - ExternalArrayBuffer 不拥有不释放原生内存（free 回调 = NULL），`detach()` 把潜在 UAF 变空缓冲
  - 符号对象 pin 住库句柄（`kLib`）防 GC 提前 dlclose；指针 pin 住 buffer
  - `FfiClosure` close 双重释放防呆（opaque 置 NULL）

## 4. Bun v1.3.14：最激进的"一等公民"信任

实现：`src/js/bun/ffi.ts`（API 层）+ `src/runtime/ffi/ffi.zig` / `FFIObject.zig`（原生）。

- `dlopen` 任意 dylib（`std.DynLib.open`，ffi.zig:1068），**无路径白名单、无权限检查**；唯一门控是编译期 `Environment.enable_tinycc`（裁 TCC，不裁 dlopen）。
- `Read.u8/i32`（FFIObject.zig:105-164）：任意 number 地址 → `@ptrFromInt` 直接解引用；`toArrayBuffer(地址, len)` = 任意地址读写窗口。软性检查仅：拒 0、u56 上限、拒 `0xDEADBEEF` 等魔法地址防 segfault。
- `--no-addons` 只挡 `process.dlopen`/N-API，**不挡 bun:ffi**；无 Deno 式 permission model（`--allow-fs` 之类不存在）。
- **比所有家都远的一步**：`cc()` 内置 TinyCC，**运行时编译 C 并调用**（`dangerouslyRunWithoutJitProtections`）——把供应链攻击面从"贴恶意 so"缩到"贴一段 C 字符串"。

## 5. 全景对比

| | PHP 8.5.7 | txiki.js | Bun 1.3.14 | TinyPHP.rt（现状） |
|---|---|---|---|---|
| 运行时门控 | `ffi.enable` 三档 + preload/scope | 无 | 仅编译期 TCC 开关 | 无 |
| dlopen 任意库 | ✓ | ✓ | ✓ | ✓（含系统库回退链） |
| 整数 → 任意地址 | `cast("int*", n)` | `createPointer(bigint)` | `@ptrFromInt(number)` | `cast(type, int)` + `getAddress()` |
| 固定数组边界检查 | ✓ | ✗ | ✗ | **✓** |
| 指针下标/算术检查 | ✗ | ✗ | ✗ | 仅拒负下标 |
| 运行时编译 C | ✗ | ✗ | ✓（TinyCC） | ✗ |
| 预处理指令 | 完整（preload 用） | N/A | N/A | 拒 `#`（ValueError）✓ |
| 回调（C→PHP/JS） | trampoline 完整 | JSCallback | JSCallback | **未实现**（少一块危险面，也少功能） |
| 威胁模型 | 多租户服务端 | 本地可信 | 本地可信 | 本地可信 |

结论：四方事实分成两派。**PHP 独自做安全**（因为它历史上跑陌生人的代码）；**txiki/Bun/tphp 同哲学**——本地运行时里 `foo.php` ≈ `foo.c`，FFI 只是省掉编译步骤的工具，"防恶意"是伪命题，工程精力花在生命周期与防崩溃上。

## 6. TinyPHP.rt 现状评估

- 实现路线（PLAN/02 §4.5，txiki mod_ffi 精简 + php-src API 语义对齐）在**其威胁模型下是正确的**：无门控不丢人，与 txiki/Bun 一致；固定数组边界检查、拒预处理、拒 bitfield 比 txiki 略严。
- **真正的变量：包管理器**。`tphp add` 装来的 vendor 代码是**第三方代码**——威胁模型已从"跑自己的脚本"滑向"自己 + 装来的"。npm/Composer 生态的默认答案是"装包=信任"，但 FFI 把这个信任直接升级成任意代码执行。
- **PHP 的方案不值得整体照搬**（preload/scope 对 CLI 工具是过度设计；`ffi.enable` 式总闸也不做——见 §7，配置面即负债），可取的是两个低成本点：cast 收紧 + 信任边界声明。

## 7. 建议（按性价比）

1. **收紧整数 → 指针：`FFI::cast` 拒绝裸 int（只接受 CData/null）** —— ✅ 已实现（2026-10-21，`tests/ffi/test-ffi-cast-no-int.php`）
   - `getAddress()` 保留（单向阀：能读出地址、不能凭空造地址）。
   - 正常路径（`FFI::new`、函数返回值视图、`addr` 配对）不受影响。
   - 做完这一条，tphp 的地址伪造面**比三方都严**——三方都把"整数伪造指针"当必要逃生门，tphp 不需要。
   - 这是 **API 语义收紧，不是配置**：零配置面，无状态组合。
2. **信任模型靠声明与边界，不靠运行时开关**
   - `doc/ffi.md` 声明："FFI 是原生扩展级权限；**装包即信任**（vendor 代码与 FFI 不可兼得）"。
   - 第三方代码的信任边界放在**包管理层**：已有的 fail-closed 覆盖检查 + **lock integrity 哈希固定**（逐文件 sha1 快照，install/build 两校验点；已实装于 [../PLAN/pkg-manager.md](../PLAN/pkg-manager.md) 阶段 E）——它回答"我装的是不是我审过的那个"，不需要密钥、豁免名单或配置面。
   - 将来若真要跑不可信代码，那是完整的 sandbox 特性（进程隔离/系统调用过滤），不是一行开关能兑现的承诺。
3. **明确不做**：
   - **`ffi.enable` 类运行时总闸**（环境变量/CLI/INI 都不做）——配置面即负债：单机 CLI 一个进程要么全可信要么全不可信，开关解决不了"一半可信"，只会复制 php.ini 大爆炸（PHP 需要它是因为同机混跑可信与陌生人代码，tphp 无此场景）。
   - **动态库特定标识/签名制**——公开标识攻击者可复制（防意外不防恶意）；签名制需要密钥体系 + 系统库豁免名单，而 FFI 的杀伤力（`system()` 等）恰好全在豁免的系统库里，一行合法调用即绕穿（同 disable_functions 教训）。
   - 指针算术/memcpy 的边界检查——`cast` 收紧后挡不住有心人，只制造虚假安全感（PHP 也没做）。
   - Bun 式运行时编译 C（`cc()`）——供应链攻击面的反面教材。
   - preload/scope 预授权——与 CLI 单机定位不符。

## 8. 参考

| 源 | 位置 |
|---|---|
| PHP FFI | php-src `ext/ffi/ffi.c`（本机 `C:\Users\kllxs\Desktop\php-src-php-8.5.7`）：`zend_ffi_validate_api_restriction` / `ZEND_FFI_VALIDATE_API_RESTRICTION` / `zend_ffi_cdata_read_dim` / `zend_ffi_create_callback` |
| txiki.js FFI | 本机 `C:\Users\kllxs\Desktop\txiki.js`：`src/mod_ffi.c`、`src/js/stdlib/ffi/ffi.js` |
| Bun FFI | 本机 `C:\Users\kllxs\Desktop\bun-bun-v1.3.14`：`src/js/bun/ffi.ts`、`src/runtime/ffi/ffi.zig`、`FFIObject.zig` |
| 本项目 | `src/ffi.c`、`doc/ffi.md`、`PLAN/02-runtime-features.md §4.5` |
