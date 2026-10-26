<?php
// async(EXPR) 在命名空间内也必须解析到绝对的 \Tphp\spawn
// （曾生成相对名 Tphp\spawn，被拼成 App\Tphp\spawn → undefined function）
namespace App;

use function Tphp\{async, await};
use Tphp\TaskGroup;

function work(int $x): int {
    usleep(1000);
    return $x * 2;
}

// 1) 调用形态：整体搬进协程
$c = async(work(21));
echo "call=", await($c), "\n";

// 2) 包装形态：async($callable) 走 \Tphp\async
$fn = async(work(...));
echo "wrap=", $fn(5)->wait(), "\n";

// 3) 任务组 thunk：$obj->spawn(EXPR) 同规则
$g = new TaskGroup();
$g->spawn(work(1));
$g->spawn(work(2));
$r = $g->wait();
sort($r);
echo "group=", implode(",", $r), "\n";

// EXPECT: call=42
// EXPECT: wrap=10
// EXPECT: group=2,4
