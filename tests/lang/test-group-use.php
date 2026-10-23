<?php
// 组 use：use function A\{f, g}; / use A\{B, C as D};
use function Tphp\{spawn, await, all};
use Tphp\{Worker, Worker as W2};

function inc2(int $x): int { return $x + 2; }

$r = all([spawn('inc2', 1), spawn('inc2', 2), spawn('inc2', 3)]);
echo "all=", implode(",", $r), "\n";

$c = spawn('inc2', 10);
echo "await=", await($c), "\n";

// 组内别名生效（不实例化，只验证解析与名字绑定）
$cls = W2::class;
echo "alias=", $cls, "\n";

// EXPECT: all=3,4,5
// EXPECT: await=12
// EXPECT: alias=Tphp\Worker
