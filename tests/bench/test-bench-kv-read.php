<?php
// bench: literal string-key map lookup x 1.6e6 (interned-key pointer-eq path)
// EXPECT: sum=7200000
$m = ["alpha" => 1, "beta" => 2, "gamma" => 3, "delta" => 4,
      "epsilon" => 5, "zeta" => 6, "eta" => 7, "theta" => 8];
$t0 = microtime(true);
$sum = 0;
for ($r = 0; $r < 200000; $r++) {
    $sum += $m["alpha"] + $m["beta"] + $m["gamma"] + $m["delta"]
          + $m["epsilon"] + $m["zeta"] + $m["eta"] + $m["theta"];
}
$ms = (microtime(true) - $t0) * 1000;
error_log(sprintf("BENCH kv-read 1.6e6 = %.1f ms", $ms));
echo "sum=$sum\n";
