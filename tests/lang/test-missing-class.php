<?php
// B5: 缺失类 = 运行期可捕获 Error（PHP 8 双引号消息对拍 q5）；死分支不报错
// EXPECT: 1: Error: Class "NoClass" not found
// EXPECT: 2: dead branch ok
// EXPECT: 3: 3: Error: Class "NoClass" not found
// EXPECT: 4: 4: Error: Class "NoClass" not found
// EXPECT: bool(false)
// EXPECT: 5: 5: Error: Class "Missing" not found
try { new NoClass(); } catch (Throwable $e) { echo "1: ", get_class($e), ": ", $e->getMessage(), "\n"; }
if (false) { new AlsoMissing(); }
echo "2: dead branch ok\n";
try { echo "3: ", NoClass::K, "\n"; } catch (Throwable $e) { echo "3: ", get_class($e), ": ", $e->getMessage(), "\n"; }
try { echo "4: ", NoClass::m(), "\n"; } catch (Throwable $e) { echo "4: ", get_class($e), ": ", $e->getMessage(), "\n"; }
var_dump(instanceof_test());
function instanceof_test() { $x = new K; return $x instanceof Missing; }
class K {}
try { echo "5: ", Missing::CONST_X, "\n"; } catch (Throwable $e) { echo "5: ", get_class($e), ": ", $e->getMessage(), "\n"; }
