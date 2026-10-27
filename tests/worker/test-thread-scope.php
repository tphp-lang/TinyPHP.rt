<?php
// EXPECT: inc-fn=42
// EXPECT: cross=ok
// EXPECT: static=77
// EXPECT: closure=7
// EXPECT: nonstatic=Error
// EXPECT: badcap=NotCloneableError
// EXPECT: static-m=5
// EXPECT: thiscap=NotCloneableError
// P1-10（PLAN/02 §Thread）：Thread 入口三形态的**作用域**——
//   1) 字符串函数名 = 全局解析（main + inc_progs）：require 进来的
//      库文件函数照样可作入口（旧实现只查主 Program，PHP 原生无此限制）
//   2) [Cls, 'method'] 静态方法：按类名+方法名在子线程重建（类表每线程克隆，
//      指针不可跨线程）
//   3) Closure 形态回归（无捕获）
//   4) 跨 Program 双向调用：子线程继承父线程 main+inc_progs 全局视图
//      （入口在 lib 调 main / 入口在 main 调 lib——main 启动时 freeze
//      早于 require，ic 必为 NULL，只能靠子线程 inc_progs 慢路径解析）
//   5) 非静态方法入口 → Error
//   6) 捕获不可序列化对象（无 __cloneState，即 $this 带 FFI 指针的症状）
//      → NotCloneableError，报错指向改用函数名/静态方法形态
//   7) 方法上下文 static function 闭包（pebview bindAsync 同款）：不捕获
//      $this，可作 spawn 入口
//   8) 方法上下文普通闭包自动捕获 $this（PHP 对齐 P1-3）→ 同 6 拒绝
use Tphp\Thread;

require "thread-scope-lib.php";

function thread_scope_main_part(): int {
    return 2;
}

function thread_scope_main_entry(): string {
    return thread_scope_lib_helper() === 'L' ? 'ok' : 'bad';
}

// 1) inc_progs 全局函数名（入口在 lib，回调 main 函数）
$t1 = new Thread();
$t1->spawn('thread_scope_worker');
$r1 = $t1->wait();
echo "inc-fn=", $r1[0], "\n";

// 2) main 函数名入口（回调 lib 函数——startup freeze 时 lib 未加载）
$t1b = new Thread();
$t1b->spawn('thread_scope_main_entry');
$r1b = $t1b->wait();
echo "cross=", $r1b[0], "\n";

// 3) 静态方法（类也在 inc_progs 里）
$t2 = new Thread();
$t2->spawn([ThreadScopeCls::class, 'stat']);
$r2 = $t2->wait();
echo "static=", $r2[0], "\n";

// 4) 闭包形态不回归
$t3 = new Thread();
$t3->spawn(fn() => 7);
$r3 = $t3->wait();
echo "closure=", $r3[0], "\n";

// 5) 非静态方法应拒绝（线程不启动）
try {
    $t4 = new Thread();
    $t4->spawn([ThreadScopeCls::class, 'inst']);
    echo "nonstatic=none\n";
} catch (\Throwable $e) {
    echo "nonstatic=", get_class($e), "\n";
}

// 6) 捕获不可序列化对象 → 明确报错（线程不启动）
try {
    $t5 = new Thread();
    $cap = new ThreadScopeCls();
    $t5->spawn(fn() => $cap);
    echo "badcap=none\n";
} catch (\Throwable $e) {
    echo "badcap=", get_class($e), "\n";
}

// 7/8) 方法上下文闭包（pebview bindAsync 同款：spawn 写在实例方法里）
class ThreadScopeSpawner {
    public function runStatic(): int {
        $t = new Thread();
        $t->spawn(static function (): int { return 5; });
        $r = $t->wait();
        return $r[0];
    }

    public function runThis(): string {
        try {
            $t = new Thread();
            $t->spawn(function (): int { return 6; });
            return "none";
        } catch (\Throwable $e) {
            return get_class($e);
        }
    }
}
$sp = new ThreadScopeSpawner();
echo "static-m=", $sp->runStatic(), "\n";
echo "thiscap=", $sp->runThis(), "\n";
