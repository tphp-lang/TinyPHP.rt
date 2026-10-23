<?php
// Coroutine::wait() —— await 的对象式别名（返回值 / 异常透传）
use function Tphp\spawn;

function add(int $a, int $b): int {
    usleep(10000);
    return $a + $b;
}

$c = spawn('add', 3, 4);
echo "wait=", $c->wait(), "\n";
echo "done=", $c->isDone() ? "yes" : "no", "\n";
echo "again=", $c->wait(), "\n";          // 幂等

$bad = spawn(function () { throw new RuntimeException("boom"); });
try {
    $bad->wait();
} catch (RuntimeException $e) {
    echo "caught=", $e->getMessage(), "\n";
}

// EXPECT: wait=7
// EXPECT: done=yes
// EXPECT: again=7
// EXPECT: caught=boom
