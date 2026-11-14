<?php
$N = 100000;
function t($name, $f) { $t0 = microtime(true); $r = $f(); $ms = (microtime(true)-$t0)*1000; error_log(sprintf("%-24s min-ish %.3f ms", $name, $ms)); return $r; }
$s1 = "aaaa"; $s2 = "bbbb";
// A: 纯循环地板
t("floor_empty", function() use ($N) { $x=0; for ($i=0;$i<$N;$i++) { $x+=$i&1; } return $x; });
// B: int ternary → var
t("int_tern", function() use ($N) { $x=0; for ($i=0;$i<$N;$i++) { $x += ($i&1) ? 1 : 2; } return $x; });
// C: str ternary → var
t("str_tern", function() use ($N,$s1,$s2) { $v="x"; for ($i=0;$i<$N;$i++) { $v = ($i&1) ? $s1 : $s2; } return strlen($v); });
// D: bool 条件 ternary（无 & 运算）
t("bool_cond_tern", function() use ($N) { $x=0; for ($i=0;$i<$N;$i++) { $x += $i ? 1 : 2; } return $x; });
// E: if/else int
t("if_else_int", function() use ($N) { $x=0; for ($i=0;$i<$N;$i++) { if ($i&1) { $x+=1; } else { $x+=2; } } return $x; });
// F: match int
t("match_int", function() use ($N) { $x=0; for ($i=0;$i<$N;$i++) { $x += match($i&1) { 1=>1, default=>2 }; } return $x; });
// G: ternary 条件取反形态
t("neg_cond_tern", function() use ($N) { $x=0; for ($i=0;$i<$N;$i++) { $x += !($i&1) ? 2 : 1; } return $x; });
echo "done\n";
