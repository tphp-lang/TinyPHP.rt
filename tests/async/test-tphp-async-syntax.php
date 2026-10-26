<?php
// Tphp\ 命名空间 + async(EXPR) 语法结构：
//   async(p1(3,4))   —— 调用表达式整体在协程里执行（不是先执行再传值）
//   $obj->spawn(EXPR) —— 同规则（spawn 是并发保留方法名）
//   async($callable)  —— 单实参不是调用形态时，按"包装"处理
use function Tphp\{async, await};
use Tphp\TaskGroup;

function add(int $a, int $b): int {
    usleep(10000);
    return $a + $b;
}

// 1) async(EXPR) 立即返回，不阻塞主流程
$t0 = microtime(true);
async(add(3, 4));
$ms = (microtime(true) - $t0) * 1000;
echo "async-nowait=", $ms < 30 ? "fast" : "slow", "\n";

// 2) 返回 Coroutine，await 取值
$c = async(add(20, 22));
echo "await=", await($c), "\n";

// 3) 自由变量按 spawn 时点捕获（之后改 $n 不影响）
$n = 5;
$c2 = async(add($n, 1));
$n = 100;
echo "captured=", await($c2), "\n";

// 4) TaskGroup::spawn(EXPR) 自动包 thunk
$g = new TaskGroup();
$g->spawn(add(1, 2));
$g->spawn(add(3, 4));
$r = $g->wait();
sort($r);
echo "group=", implode(",", $r), "\n";

// 5) async($callable) 仍是包装形态（实参不是调用表达式）
$fn = async(add(...));
echo "wrapped=", $fn(10, 20)->wait(), "\n";

// EXPECT: async-nowait=fast
// EXPECT: await=42
// EXPECT: captured=6
// EXPECT: group=3,7
// EXPECT: wrapped=30
