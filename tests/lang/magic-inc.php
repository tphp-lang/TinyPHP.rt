<?php
// included by test-magic-constants.php — __FILE__/__DIR__/__LINE__ must
// reflect THIS file, not the main script (parse-time folding).
echo "inc-line=", __LINE__, "\n";
$f = __FILE__;
echo "inc-file-ok=", (strpos($f, "magic-inc.php") !== false) ? "true" : "false", "\n";
$d = __DIR__;
echo "inc-dir-ok=", (strpos($d, "lang") !== false) ? "true" : "false", "\n";
