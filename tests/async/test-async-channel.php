<?php
// M4-c: Tphp\Channel —— 无界/有界、send/recv 挂起交互、close 语义、len、
// await(Channel)
use function Tphp\spawn;
use function Tphp\await;

// 1) 无界：send 不挂起，recv 按序取
$ch = new Tphp\Channel();
$ch->send("a");
$ch->send("b");
var_dump($ch->recv());
var_dump($ch->len());

// 2) 跨协程：空 recv 挂起 → send 交接
$ch2 = new Tphp\Channel();
spawn(function () use ($ch2) { $ch2->send("from-coro"); });
var_dump(await($ch2));

// 3) 有界：满挂 send，recv 解锁
$b = new Tphp\Channel(1);
$b->send("first");
$c = spawn(function () use ($b) {
    $v = $b->recv();          // 取走 first，解锁 main 的 second
    $v2 = $b->recv();         // second 在缓冲
    return $v . "|" . $v2;
});
$b->send("second");           // 满 → 挂起，被上面的 recv 唤醒
var_dump(await($c));
var_dump($b->len());
// 4) close：缓冲值可取尽，之后 null；send 抛错
$cl = new Tphp\Channel();
$cl->send("x");
$cl->close();
var_dump($cl->recv());
var_dump($cl->recv());
try { $cl->send("y"); } catch (Throwable $e) {
    echo "send-closed: ", $e->getMessage(), "\n";
}

// 5) close 唤醒空 recv → null
$cl2 = new Tphp\Channel();
spawn(function () use ($cl2) {
    $cl2->close();
});
var_dump(await($cl2));

// EXPECT: string(1) "a"
// EXPECT: int(1)
// EXPECT: string(9) "from-coro"
// EXPECT: string(12) "second|first"
// EXPECT: int(0)
// EXPECT: string(1) "x"
// EXPECT: NULL
// EXPECT: send-closed: send to a closed channel
// EXPECT: NULL
