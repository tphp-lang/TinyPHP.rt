# tests/php-compat — php-src 对拍层（PLAN/04 §2）

从 php-src `Zend/tests` 抽 `.phpt` 子集，转成 `test-*.php` + `// EXPECT:` 格式，
与 tphp 输出逐例比对。断言协议借鉴 `run-tests.php` 的 EXPECT。

## 转换

```powershell
powershell -File tools/convert-phpt.ps1 -Src <php-src>/Zend/tests -Dst tests/php-compat -Limit 80
```

只收 **纯 `--EXPECT--`**（丢弃 EXPECTF/EXPECTREGEX/SKIPIF）且 <1.5KB 的小文件；
转换后跑一遍，**只保留 tphp 当前能全绿的**——失败的是能力缺口，不入库。

## 现状（种子）

17 例通过：`match` / `enum __call` / `anon class` / `return_types` 基础。
失败用例丢弃，待能力补齐后重新转换入库。

## 扩大覆盖

1. 提高 `-Limit`，按目录筛（`operators` / `array` / `heredoc` / `grammar`）
2. 补 `EXPECTF` 支持（`%s`/`%d` 通配）到转换器
3. 每修一个能力缺口 → 重跑转换 → 新绿用例入库

## 约定

- 文件头注释注明来源 `.phpt`
- 纯 ASCII（UTF-8 无 BOM）
- 由 `tests/run.ps1` 自动纳入全量回归
