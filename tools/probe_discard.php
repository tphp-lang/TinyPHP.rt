<?php
class D { static $n = 0; function __destruct(){ D::$n++; } }
function mkd(){ return new D(); }
function t_disc(){ $a = 5; mkd(); return 1; }   // 调用结果丢弃
for ($i=0; $i<300; $i++) $r = t_disc();
echo "discard=", D::$n, "/300\n";
