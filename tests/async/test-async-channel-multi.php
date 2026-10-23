<?php
// Channel 多等待者：多消费者（信号量）、多生产者、close 唤醒全部
use function Tphp\spawn;
use function Tphp\all;

// 1) 信号量：4 个协程抢 2 个令牌（单等待者模型下曾 SIGSEGV）
$sem = new Tphp\Channel(2);
$sem->send(true);
$sem->send(true);
$tasks = [];
foreach ([1, 2, 3, 4] as $id) {
    $tasks[] = spawn(function () use ($id, $sem) {
        $sem->recv();
        try {
            usleep(5000);
            return $id * 10;
        } finally {
            $sem->send(true);          // 归还令牌
        }
    });
}
$r = all($tasks);
sort($r);
echo "sem=", implode(",", $r), "\n";

// 2) 多生产者 → 单消费者
$ch = new Tphp\Channel(2);
$ps = [];
for ($i = 1; $i <= 3; $i++) {
    $ps[] = spawn(function () use ($ch, $i) {
        usleep($i * 2000);
        $ch->send($i);
    });
}
$got = [];
for ($k = 0; $k < 3; $k++) { $got[] = $ch->recv(); }
all($ps);
sort($got);
echo "prod=", implode(",", $got), "\n";

// 3) close 唤醒全部等待者（各自拿到 null）
$c2 = new Tphp\Channel(0);
$ws = [];
for ($i = 0; $i < 3; $i++) {
    $ws[] = spawn(function () use ($c2) { return $c2->recv(); });
}
usleep(5000);
$c2->close();
$nulls = all($ws);
$ok = ($nulls[0] === null && $nulls[1] === null && $nulls[2] === null);
echo "close=", $ok ? "null" : "bad", "\n";

// EXPECT: sem=10,20,30,40
// EXPECT: prod=1,2,3
// EXPECT: close=null
