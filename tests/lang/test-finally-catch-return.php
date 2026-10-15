<?php
// EXPECT: f1=F1
// EXPECT: c
// EXPECT: f2=T
// EXPECT: F2
// EXPECT: end
// EXPECT: f3=E
// EXPECT: F3
// EXPECT: r3
// EXPECT: F4
// EXPECT: NULL
// EXPECT: F5
// EXPECT: caught
// P2-A: return inside catch combined with finally（与 PHP 8.5 对拍）
function f1() { try { throw new Exception("x"); } catch (Exception $e) { return "c"; } finally { echo "F1\n"; } }
echo "f1=", f1(), "\n";
function f2() { try { echo "T\n"; } catch (Exception $e) { return "no"; } finally { echo "F2\n"; } return "end"; }
echo "f2=", f2(), "\n";
function f3() { try { throw new Exception("y"); } catch (Exception $e) { echo "E\n"; return "r3"; } finally { echo "F3\n"; } }
echo "f3=", f3(), "\n";
function f4() { try { throw new Exception("z"); } catch (Exception $e) { return; } finally { echo "F4\n"; } }
$f = f4(); var_dump($f);
function f5() { try { throw new Exception("q"); } catch (Exception $e) { throw $e; } finally { echo "F5\n"; } }
try { f5(); } catch (Exception $e) { echo "caught\n"; }
