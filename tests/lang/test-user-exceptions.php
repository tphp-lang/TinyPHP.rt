<?php
// 用户异常类体系（2026-10-15 P1-2 回归）
// 覆盖：extends 内置异常 / catch 用户类 / catch 基类 / catch Throwable /
// 子类被子类 catch / class_exists / instanceof / 链式 previous /
// 构造器提升 + parent::__construct / enum from() 抛 ValueError /
// ctor 抛异常不落地。历史上：extends 报 Class not found、catch 用户类报
// Unknown exception class、class_exists('Exception')=false。
// EXPECT: my:my:boom 7 boom
// EXPECT: as-exception:MyErr my:m
// EXPECT: as-throwable:MyErr
// EXPECT: status:404 http
// EXPECT: y
// EXPECT: y
// EXPECT: chain
// EXPECT: sub-as-myerr:my:sub
// EXPECT: valueerror-ok
// EXPECT: ctor-caught:inner-throw
class MyErr extends Exception {
    public function __construct(private string $ctx) {
        parent::__construct("my:$ctx", 7);
    }
    public function ctx(): string { return $this->ctx; }
}
try { throw new MyErr("boom"); }
catch (MyErr $e) { echo "my:", $e->getMessage(), " ", $e->getCode(), " ", $e->ctx(), "\n"; }

try { throw new MyErr("m"); }
catch (Exception $e) { echo "as-exception:", get_class($e), " ", $e->getMessage(), "\n"; }
try { throw new MyErr("m2"); }
catch (Throwable $t) { echo "as-throwable:", get_class($t), "\n"; }

class HttpError extends RuntimeException {
    public function __construct(public int $status) { parent::__construct("http"); }
}
try { throw new HttpError(404); }
catch (RuntimeException $e) { echo "status:", $e->status, " ", $e->getMessage(), "\n"; }

echo class_exists('Exception') ? 'y' : 'n', "\n";
$m = new MyErr("x");
echo $m instanceof Throwable ? 'y' : 'n', "\n";

class PlainErr extends Exception {}
$inner = new PlainErr("chain");
$outer = new PlainErr("outer", 1, $inner);
echo $outer->getPrevious()->getMessage(), "\n";

class MySub extends MyErr {}
try { throw new MySub("sub"); }
catch (MyErr $e) { echo "sub-as-myerr:", $e->getMessage(), "\n"; }

enum S: string { case A = 'a'; }
try { S::from('zzz'); echo "no\n"; }
catch (ValueError $e) { echo "valueerror-ok\n"; }

class BadCtor extends Exception {
    public function __construct() { throw new RuntimeException("inner-throw"); }
    public function __destruct() { echo "never-dtor\n"; }
}
try { $x = new BadCtor(); echo "no\n"; }
catch (Exception $e) { echo "ctor-caught:", $e->getMessage(), "\n"; }
