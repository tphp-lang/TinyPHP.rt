<?php
// 跨帧异常传播（2026-10-15 审计 P0-1 回归）
// 历史 bug：调用只要是 try 体最后一条 op，调用方 catch 整段错过 handler
// （ex 表 end_pc 排他 + DO_FCALL 压帧前推 pc 的差一错误）。
// EXPECT: fn:cross-fn
// EXPECT: method:cross-method
// EXPECT: closure:cross-closure
// EXPECT: deep:deep
// EXPECT: builtin:ArgumentCountError
// EXPECT: notlast:mid
// EXPECT: finally:fin-msg
// EXPECT: finally-ran
// EXPECT: static:static

function boomFn() { throw new Exception('cross-fn'); }
class Boom {
    public function m() { throw new Exception('cross-method'); }
    public function deep3() { lvl2(); }
}
function lvl2() { lvl1(); }
function lvl1() { throw new Exception('deep'); }
function boomBuiltin() { strlen(1, 2, 3); }
function boomFin() { throw new Exception('fin-msg'); }

// 1. 函数调用是 try 体最后一条 op
try { boomFn(); } catch (Exception $e) { echo 'fn:', $e->getMessage(), "\n"; }

// 2. 方法调用是 try 体最后一条 op
$b = new Boom();
try { $b->m(); } catch (Exception $e) { echo 'method:', $e->getMessage(), "\n"; }

// 3. 闭包调用是 try 体最后一条 op
$c = function () { throw new Exception('cross-closure'); };
try { $c(); } catch (Exception $e) { echo 'closure:', $e->getMessage(), "\n"; }

// 4. 三层深层调用
try { $b->deep3(); } catch (Exception $e) { echo 'deep:', $e->getMessage(), "\n"; }

// 5. builtin 报错跨帧（Error 家族）
try { boomBuiltin(); } catch (Error $e) { echo 'builtin:', get_class($e), "\n"; }

// 6. 调用不是 try 体最后一条 op（原本就能接住，锁住不回归）
function boomMid() { throw new Exception('mid'); }
try { boomMid(); echo "never\n"; } catch (Exception $e) { echo 'notlast:', $e->getMessage(), "\n"; }

// 7. 跨帧 + finally：catch 与 finally 都该跑
try { boomFin(); } catch (Exception $e) { echo 'finally:', $e->getMessage(), "\n"; } finally { echo "finally-ran\n"; }

// 8. 静态方法跨帧
class SBoom { public static function s() { throw new LogicException('static'); } }
try { SBoom::s(); } catch (LogicException $e) { echo 'static:', $e->getMessage(), "\n"; }
