<?php
// 引擎回归：native 类 self 返回链 + main frame 存活到自然退出（bug #2 收编）
// 病史：teb Web::start() 原实现 addref 一次两用（g_teb 全局份 + 返回值转移份），
// real_stop 释放 g_teb 时吃掉 main frame 返回值 TMP 槽的合法份额 → 自然退出
// 二次 free（EXIT=139/0xC0000005）。teb.c 修复后本脚本应自然退出 EXIT=0。
// 无 teb 链入时输出 skip → EXPECT-SKIP 计 skipped（CI 源码构建无本地 ext）。
// EXPECT-SKIP: skip
if (!class_exists("Tphp\\Teb\\Web")) {
    echo "skip\n";
    exit(0);
}
use Tphp\Teb\Web;
$web = new Web();
$web->route("/ping", fn(Request $r) => new Response("pong\n"));
$web->start(["listening_ports" => "18161", "num_threads" => "2"]);
$web->stop();
echo "done\n";
// EXPECT: done
