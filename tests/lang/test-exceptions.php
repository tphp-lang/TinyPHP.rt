<?php
// EXPECT-EXIT: 255
// EXPECT: msg:boom code:42 line:10
// EXPECT: R:fin-first
// EXPECT: 1
// EXPECT: prev:RuntimeException
// EXPECT: thr
// EXPECT: err caught
// EXPECT: F2
try { throw new Exception("boom", 42); }
catch (Exception $e) { echo "msg:", $e->getMessage(), " code:", $e->getCode(), " line:", $e->getLine(), "\n"; }
function f() { try { return 1; } finally { echo "fin-first\n"; } }
echo "R:", f(), "\n";
try {
    try { throw new RuntimeException("inner"); }
    catch (RuntimeException $e) { throw new LogicException("outer", 0, $e); }
} catch (LogicException $e) {
    echo "prev:", get_class($e->getPrevious()), "\n";
}
try { throw new Exception("u"); } catch (Throwable $t) { echo "thr\n"; }
try { strlen(1, 2, 3); } catch (Exception $e) { echo "no\n"; } catch (Error $e) { echo "err caught\n"; }
try { throw new Exception("x"); } finally { echo "F2\n"; }
