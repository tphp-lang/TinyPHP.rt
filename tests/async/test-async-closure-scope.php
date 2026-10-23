<?php
// 协程根帧的作用域：闭包在类方法内定义时，$this 的 private/protected
// 成员必须按声明类可见（根帧 vis_scope/called_cls 曾漏设 → 读取失败）
use function Tphp\{spawn, await};

class Counter {
    private int $n = 40;
    protected int $step = 2;

    public function runInCoroutine(): int {
        $c = spawn(function () {
            return $this->n + $this->step;   // private + protected
        });
        return await($c);
    }

    public function staticBinding(): string {
        $c = spawn(function () { return static::class; });
        return await($c);
    }
}

$obj = new Counter();
echo "sum=", $obj->runInCoroutine(), "\n";
echo "static=", $obj->staticBinding(), "\n";

// 协程内未捕获异常的文件归属（根帧 file 曾读到已释放填充值）
$bad = spawn(function () { throw new RuntimeException("boom"); });
try {
    await($bad);
} catch (RuntimeException $e) {
    echo "msg=", $e->getMessage(), "\n";
}

// EXPECT: sum=42
// EXPECT: static=Counter
// EXPECT: msg=boom
