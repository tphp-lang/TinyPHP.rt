# bug：字符串位运算（`^` `&` `|` `~`）缺 both-strings 分支，返回 int 而非逐字节字符串

> 状态：**已修**（2026-11-14，主窗口会话；commit 41daece）
> 严重度：中高——静默错误（无告警、结果恒 0/-1），真实业务（二进制协议掩码、
> 简单加解密、hash 变体）必踩；teb 冒烟客户端已被坑并导致一次误诊。

## 修复记录（2026-11-14 主窗口）

- 实现：[src/value.c](../src/value.c) `str_bitwise`（bin 两串分支）+
  `tval_bit_not` 字符串分支；`tval_band/bor/bxor` 前置 `T_STR&&T_STR`
  分流。VM（vm.c BIN_HANDLER）与 JIT（jit.c call_slow_binop）共用本层
  单点修复；JIT INT 快线有 `ta==JT_INT && tb==JT_INT` 类型守卫，字符串
  恒走慢线，无双路径面。
- **规格更正（报告此处有误）**：PHP 的 `^` 不同长时取 **MIN 截断**而非
  本报告原写的「max 补齐」——zend_operators.c `bitwise_xor_function`
  源码 `MIN(s1->len,s2->len)`，PHP 8.5.1 实测 `"" ^ "ab"` = 空串、
  `"abcd" ^ "ab"` = `"\x00\x00"`。（`|` 才是 MAX 补齐；`&` MIN 截断
  与本报告一致。）
- 回归：[tests/standard/test-bitwise-str.php](../tests/standard/test-bitwise-str.php)
  22 行（报告 5 行 + & 截断 + | 补齐 + 复合赋值 + 空串 + NUL 二进制
  安全 + `^` MIN 实证），EXPECT 三方实测（tphp JIT=1/JIT=0 与 PHP
  8.5.1）逐字节一致后取值；全量套件 250/250 通过。
- 附带发现：tphp 无 `dechex` 内建（探针被迫查表法实现 hex 打印），
  记录待办不在本修范围。

## 症状

两个操作数**均为字符串**时，位运算符全部经 `tval_to_int` 收缩成整数运算：
非数字字符串被转成 0，返回值是 **int**（PHP 应返回逐字节 XOR 的 **string**）。

当前引擎实测（`build/tphp.exe run`，2026-11-14 build）：

| 表达式 | 引擎当前行为 | PHP 8 正确行为 |
|---|---|---|
| `"p" ^ "\x11"` | `int 0` | `string(1) "\x61"`（即 `"a"`） |
| `"abc" ^ "ABC"` | `int 0` | `string(3) "\x20\x20\x20"` |
| `~"ab"` | `int -1`（to_int=0 → ~0） | `string(2) "\x9e\x9d"` |
| `"5" ^ 2` | `int 7` | `int 7`（**混合 string/int 已正确**） |
| `"1" ^ "2"` | `int 3`（数字串 to_int 可用） | `string(1) "\x03"` |

无任何 warning/TypeError——**静默错误**。

## 复现

```php
$c = "p" ^ "\x11";
echo gettype($c);          // 引擎: integer；PHP: string
echo $c;                   // 引擎: 0；PHP: a
```

附加陷阱（teb 事故路径）：运算结果 int 参与 `.` 拼接时拼进的是**十进制文本**，
且 mask 串里恰好含可打印数字字符（`"\x33"` 即 `'3'`，to_int=3）时结果混乱度
更高——实测 teb 旧客户端掩码拼出 `"003001"` 这类 ASCII 数字串。

## 取证链

- 实现位置：[src/value.c:406-408](../src/value.c) `tval_band/bor/bxor` 与
  [src/value.c:423](../src/value.c) `tval_bit_not`，一律
  `num_result_i(tval_to_int(a) OP tval_to_int(b))`，**无字符串分支**。
- JIT 共享同层：[src/jit.c:136](../src/jit.c) `OP_BW_XOR → tval_bxor` ——
  **改 value.c 即解释器/JIT 双后端同修**。
- 返回类型 int 实锤：带 `: string` 参数类型的 helper 收到
  `TypeError: ... int given`。
- 混合类型（一 string 一 int）路径已符合 PHP（转数字走整数运算），修复只需
  **both-strings 分支**，不要动数字路径。
- 现有测试缺口：tests/jit/test-jit-bwfast-001.php 仅整数路径；tests/ 无
  字符串位运算覆盖。

## 真实事故（动机）

teb WS 冒烟客户端 `tests/wsclient.php` 掩码拼装
`$masked . ($payload[$i] ^ $maskKey[$i % 4])`：逐字符得到 int（0/3…），
拼接成十进制 ASCII 文本 → WS 帧**载荷与帧长头不符** → 整条流错位 →
服务端 onMessage 收到乱码。该现象曾被误诊为「civetweb data 回调未解 mask」
（错案已澄清，见 teb 仓 `doc/P1-design.md` 教训节 + commit `0b4d301`；
客户端已用 `chr(ord() ^ ord())` 规避）。

## 修复方向（供分析取舍）

PHP 语义（zend_operators.c bitwise_string_function 族）：

1. **两操作数均 string** 才走字符串路径：
   - `^` / `|`：结果长度 = max(len)，短侧按 `\0` 补齐逐字节运算；
   - `&`：结果长度 = **min(len)**（截断，超出部分 AND `\0` 恒 0）；
   - `~`：逐字节取反，长度不变（unary，同样缺分支）。
2. 结果构造参照现有字符串结果路径（tstr_new/tm_malloc），注意返回值
   转移份 addref 约定（见 doc/ext-dev.md 所有权节）。
3. 回归测试建议放 tests/standard（如 test-bitwise-str.php），覆盖上表
   5 行 + `&` 截断 + `|` 补齐 + 非 ASCII 字节（二进制安全）。

## 关联

- teb 仓 0b4d301（客户端规避 + 教训记录）
- 本目录 2026-11-14-winpthread-dll-clock_gettime64.md（同会话发现的另一项）
