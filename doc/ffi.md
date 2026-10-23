# FFI

设计规格：[../PLAN/02-runtime-features.md §4.5](../PLAN/02-runtime-features.md)。
安全分析（PHP/txiki/Bun 三方对照与建议）：[ffi-security.md](ffi-security.md)。

通过 **libffi** 调用 C 动态库，API 语义对齐 PHP `ext/ffi`。

## 快速开始

```php
// 系统库零配置（Windows 自动回退 kernel32/msvcrt 等；Linux RTLD_DEFAULT）
$ffi = FFI::cdef("
    int abs(int j);
    unsigned int GetTickCount(void);
    int sprintf(char *str, const char *format, ...);
", "msvcrt.dll");   // Linux 可省略第二参或写 libc

echo $ffi->abs(-5);              // 5

// CData：原生内存视图
$p = FFI::new("int[4]");
$p[0] = 42;
echo $p[0];

$s = FFI::new("char[64]");
$ffi->sprintf($s, "hi %d", 7);   // 变参
```

## API

### FFI（静态）

| 方法 | 说明 |
|---|---|
| `FFI::cdef($code, $lib = null)` | 解析 C 声明，返回句柄对象；`$lib` 指定 DLL/so |
| `FFI::new($type[, $owned = true])` | 分配 CData（`"int[4]"` / `"struct P"` / `"void*"`） |
| `FFI::free($ptr)` | 释放 owned CData |
| `FFI::memcpy($dst, $src, $n)` | 原生内存拷贝 |
| `FFI::sizeof($typeOrCData)` | 类型/对象字节大小 |
| `FFI::alignof($type)` | 对齐 |
| `FFI::cast($type, $ptr)` | 指针重解释为 CData 视图；`$ptr` 只接受 CData / null（**拒裸 int**，见 ffi-security.md §8） |

### CData（实例）

- **数组下标**：`$p[i]` 读写（指针/数组类型）
- **struct 字段**：`$s->x` 读写（含嵌套 struct/指针字段）
- `getValue()` / `setValue($v)` —— 标量解引用
- `cast($type)` / `getAddress()` / `getSize()`

### FFI 实例

- `$ffi->funcName(...)` —— 按 cdef 声明调用
- `$ffi->CONST_NAME` —— enum 常量
- `$ffi->new/sizeof/offsetof` —— 带类型作用域的实例方法

## cdef 支持的子集

- 函数声明、基本类型、指针、平铺 struct/enum、简单 typedef、变参 `...`
- **函数指针**（回调签名）：`int (*compar)(const void *, const void *)`（参数位置；v1 不支持 struct 字段/数组元素赋值形态）
- **拒绝**：`#include`/`#define` 等预处理（`ValueError`）
- **拒绝**：bitfield（union 解析已支持布局，按值 ABI 用最大成员近似）
- 不支持：完整 C 类型系统、变参回调、数组参数

## 回调（C 函数指针 → PHP 闭包）

闭包直接传给函数指针类型的参数，**隐式**建 libffi trampoline（无新类型）：

```php
$libc = FFI::cdef("
    void qsort(void *base, size_t nmemb, size_t size,
               int (*compar)(const void *, const void *));
", "msvcrt.dll");

$a = FFI::new("int[3]");
$a[0] = 9; $a[1] = 1; $a[2] = 5;
$libc->qsort($a, 3, 4, function ($x, $y) {
    return $x->cast("int*")[0] <=> $y->cast("int*")[0];  // void* 先 cast 再解引用
});
```

语义（设计规格：[../PLAN/02-runtime-features.md §4.5.1](../PLAN/02-runtime-features.md)，四家对照分析见 [ffi-security.md](ffi-security.md)）：

| 项 | 行为 |
|---|---|
| 参数编组 | 标量直转；`char*`/`int8*` → string **拷贝**（防悬空）；其他指针/`void*` → 非 owned CData 视图；struct → CData 别名（**回调返回后失效**） |
| 返回值 | 标量宽松转换；同尺寸 CData memcpy；**类型不匹配 → 警告 + 零值** |
| 生命周期 | trampoline + 闭包保活到**脚本/线程结束**（传给 C 后长期持有安全，如 qsort）；无显式 free |
| **回调内 throw** | **硬 Fatal**（"Throwing from FFI callbacks is not allowed"，不可 catch）——unwind 不能穿过 ffi_call 的 C 栈帧 |
| 重入 | 回调内允许再调 FFI |
| 线程 | 同线程同步；C 跨线程调回调 = 未定义（文档禁止）；多线程场景把整个 FFI 交互放 Worker |
| 地址面 | trampoline 不暴露裸地址（配合 `cast` 拒裸 int） |
| v2 后置 | struct 字段/数组元素的闭包赋值形态、同闭包 trampoline 去重、延迟异常传播（可 catch 版）——见 PLAN/02 §4.5.1 |

## 内存与生命周期

- `FFI::new(..., owned=true)` → CData 被 GC 回收时自动 `free` 原生缓冲
- 字段/下标取出的视图默认 **非 owned**，不重复释放
- `FFI::free` 显式提前释放（owned 才允许）

## 并发约定（重要）

**FFI 调用是阻塞 native 调用**——不挂起调度器，事件循环停摆。

- CPU 密集 / 长阻塞调用 → 放 **Worker 线程**
- 短调用（如 `abs`、`GetTickCount`）主线程可接受

```php
$w = new Tphp\Worker(function () {
    $me = Tphp\Worker::self();
    $ffi = FFI::cdef("int abs(int j);", "msvcrt.dll");
    $me->post($ffi->abs(-8));
});
echo $w->recv();
$w->stop();
```

## 搜索链

| 平台 | 库查找 |
|---|---|
| Windows | 指定 DLL → kernel32/user32/msvcrt 等回退 → 主模块；打包内相对路径命中清单 → exe 同级等效绝对路径加载（`LOAD_WITH_ALTERED_SEARCH_PATH`，依赖走应用目录默认搜索） |
| Linux/macOS | 指定 `.so`/`.dylib` → `RTLD_DEFAULT` |

## 测试

`tests/ffi/` —— 结构体/union/owned/拒绝预处理/系统库（部分 Windows-only，非 Windows 自动 SKIP）。
