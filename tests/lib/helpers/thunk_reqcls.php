<?php
// Bug B 辅助（2026-11-04）：动态 require 的独立 Program 里带 thunk 默认值的类
namespace Thrq;
enum Feat: int { case Default = 0; }
class Web2 {
    public $v;
    public function __construct(Feat $x = Feat::Default) { $this->v = $x->value; }
}
