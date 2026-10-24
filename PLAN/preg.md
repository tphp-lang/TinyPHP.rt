# preg · 正则（vendor pcre2）

> 状态：**P1–P4 全部完成**（2026-10-24，217/217）。后续候选：`preg_replace_callback`（回调基建已有）、数组形态 replace、PCRE2 JIT 启用、`pcre.jit` 等 INI 面。
> 语义基准：php-src 8.5.7 `ext/pcre/php_pcre.c`（探索记录见会话日志）。
> 性能路线关联：[perf-roadmap.md](perf-roadmap.md) §3 —— preg_* 是"语法阈值"最后一块；无它 benchmark 不代表真实负载。

## 1. 定位与决策

| 决策点 | 结论 |
|---|---|
| 引擎 | **vendor `deps/pcre2/`**（直接拷贝 php-src 8.5.7 `ext/pcre/pcre2lib`，含其 config.h——版本与语义基准天然对齐） |
| JIT | v1 **不启用** PCRE2 JIT（不调 `pcre2_jit_compile`；源文件照编译省配置）；后置开启 |
| 模式缓存 | 进程级编译缓存（模式串 → pcre2_code*；PHP 同款 `pcre_get_compiled_regex_cache` 思路），TLS 或进程级+互斥——单线程 CLI 先 TLS 简单版 |
| 错误模型（关键决策） | **编译失败/坏分隔符/未知修饰符 → 异常**（PHP 的 E_WARNING 按 tphp"诊断即异常"既定规则升级，同 fopen 失败路径哲学）；**运行时 backtrack/recursion limit → 保留 PHP 返回值协议**（match→false、replace→null、split→已切部分、grep→部分结果）+ `preg_last_error` 可读（PHP 此路径本无 Warning，是返回值协议不是诊断） |
| v1 函数集 | `preg_match` / `preg_match_all` / `preg_replace` / `preg_split` / `preg_grep` / `preg_last_error` / `preg_last_error_msg` |
| 后置 | `preg_replace_callback`（回调基建已有可低成本补）、`preg_replace_callback_array`、`preg_quote`（无正则，可随时补）、JIT 启用、INI 面（backtrack_limit 等 v1 用 PHP 默认值常量） |

## 2. 模式语法（对齐 php_pcre.c）

- 分隔符：跳过前导空白取首字符；字母数字/`\`/NUL → 异常 "Delimiter must not be alphanumeric…"；成对 `([{<`→`)]}>` 按括号计数找结束，单字符遇未转义止；无结束 → "No ending delimiter"
- 修饰符：`i m n s x A D U u J`（映射见 §4）；`S X` 与空白 no-op；`e` 及未知 → 异常 "Unknown modifier"
- 默认 compile options 含 `PCRE2_ALT_BSUX|PCRE2_EXTENDED_MORE|PCRE2_LITERAL_ISOLATED|PCRE2_MATCH_UNSET_BACKREF`（php_pcre.c L820+）

## 3. 函数语义（v1 范围）

### preg_match($pattern, $subject, &$matches = null, $flags = 0, $offset = 0)
- 返回 1/0；运行时 limit 错误 → false
- `$matches`：`[0]`=整匹配，`[1..n]`=捕获组；未匹配组默认 `""`；`PREG_UNMATCHED_AS_NULL` 填 null；尾部未参与组默认不插入
- 命名组：数字索引 + 同名键（DUPNAMES 优先有值者）
- `$offset` 字节偏移；负数从尾；越界 → false（BAD_UTF8_OFFSET_ERROR）
- `PREG_OFFSET_CAPTURE`：每项 `[str, off]`，未匹配 `[null, -1]`（UNMATCHED_AS_NULL）或 `["", -1]`

### preg_match_all($pattern, $subject, &$matches, $flags = PREG_PATTERN_ORDER, $offset = 0)
- 默认 PATTERN_ORDER（`[组][]` 聚合）；`PREG_SET_ORDER`（`[match][组]`）

### preg_replace($pattern, $replacement, $subject, $limit = -1)
- pattern/replacement/subject 可数组（串 pattern + 数组 replacement → TypeError）；多 pattern 链式
- 反向引用仅 `$n` / `${n}`（两位数字）；`\` 为字面转义（**不支持 `\1`**）；`$0`=整匹配；未匹配组展开为空
- limit：≤0 → 无限；无匹配返回原串副本；运行时错误 → null

### preg_split($pattern, $subject, $limit = -1, $flags = 0)
- 正则分隔符；limit 0 同 -1；flags：`NO_EMPTY`/`DELIM_CAPTURE`/`OFFSET_CAPTURE`；运行时错误 → 已切部分

### preg_grep($pattern, $array, $flags = 0)
- 原键=>元素；`PREG_GREP_INVERT` 取反；运行时错误 → 已累积结果

### preg_last_error / preg_last_error_msg
- 线程局部 error_code；msg 固定串（NO_ERROR/BACKTRACKLIMIT/RECURSION/JIT_STACKLIMIT/LIMIT_UTF8/INTERNAL）

## 4. PCRE2 选项映射

| 修饰符 | PCRE2 |
|---|---|
| i | CASELESS |
| m | MULTILINE |
| n | NO_AUTO_CAPTURE |
| s | DOTALL |
| x | EXTENDED |
| A | ANCHORED |
| D | DOLLAR_ENDONLY |
| U | UNGREEDY |
| u | UTF + UCP |
| J | DUPNAMES |

match 侧：UTF 模式开 `PCRE2_NO_UTF_CHECK` 关闭前不预校验 subject（对齐 PHP）；空匹配模拟 `/g` 用 `NOTEMPTY_ATSTART|ANCHORED`。limits：backtrack=1e6、recursion=1e5（PHP INI 默认）。

## 5. 实施分批

| 批 | 内容 | 验收 |
|---|---|---|
| **P1 基础** | pcre2 进 build（cmd/sh）；模式解析（分隔符+修饰符）+ TLS 编译缓存；`preg_match`（matches/flags/offset 基础）+ `preg_last_error[_msg]`；错误模型（编译=异常、运行=返回值+last_error） | test-preg-match.php |
| **P2 match 完整** | 命名组、UNMATCHED_AS_NULL、OFFSET_CAPTURE、match_all 两序 | test-preg-match-flags.php / test-preg-match-all.php |
| **P3 replace/split/grep** | `$n` 引用、数组形态、limit；split flags；grep invert | test-preg-replace.php / test-preg-split.php / test-preg-grep.php |
| **P4 收尾** | doc/stdlib.md 清单、PLAN 勾销、负向（坏分隔符/坏修饰符/编译失败→异常） | test-preg-errors.php |

## 6. 构建

- `deps/pcre2/`：php pcre2lib 原样（config.h 已配好）；build.cmd/sh 增加 `deps/pcre2/*.c` 编译（含 JIT 源文件——v1 不调用 jit_compile，无运行时影响）
- `ext/pcre/pcre.c`（PHP 可见函数注册进 builtin 表，ext 布局同 standard）
- 注意 `-Ideps/pcre2`（pcre2.h）；勿与 CRT 头冲突（pcre2_intmodedep.h 等内部头自洽）

## 7. 参考

- php-src 8.5.7：`ext/pcre/php_pcre.c`（语义）、`ext/pcre/pcre2lib/`（引擎源）
- 会话日志 2026-10-24「深挖 PHP preg_* 语义」
