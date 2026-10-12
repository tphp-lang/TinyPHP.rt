<?php
// EXPECT: hello 5 and 5x
// EXPECT: raw $n
// EXPECT: indented
$n = 5;
$s = <<<EOT
hello $n and {$n}x
EOT;
echo $s, "\n";
$now = <<<'EOT'
raw $n
EOT;
echo $now, "\n";
$v = <<<EOT
  indented
  EOT;
echo $v, "\n";
