<?php
// M4-f: http_get https:// —— mbedtls TLS + 内嵌 CA bundle（证书链校验）
// + chunked / TLS 协程并发 + 失败路径
// （正向路径走外网 example.com——CI flaky 教训：勿用 httpbin.org 等不稳端点）
use function Tphp\spawn;
use function Tphp\await;

// 1) TLS 协程并发：两个 https 同时握手/传输，挂起期间调度器正常推进
//    （两个连接都打 example.com——并发/TLS 覆盖不变，去掉外网不稳定依赖）
$f1 = spawn(function () { return http_get("https://example.com/"); });
$f2 = spawn(function () { return http_get("https://example.com/"); });
$b1 = await($f1);
$b2 = await($f2);
var_dump(is_string($b1) && strpos($b1, "Example Domain") !== false);
var_dump(is_string($b2) && strpos($b2, "Example Domain") !== false);

// 2) 串行 https（main 协程直连）
$b3 = http_get("https://example.com/");
var_dump(is_string($b3) && strpos($b3, "Example Domain") !== false);

// 3) 失败路径：https 到拒绝端口 → warning + false
var_dump(http_get("https://127.0.0.1:1/x") === false);

echo "tls-done\n";

// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: bool(true)
// EXPECT: tls-done
