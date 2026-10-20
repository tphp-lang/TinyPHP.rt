<?php
// EXPECT-EXIT: 255
// readonly + hooks → 加载期 Parse error（stderr 不参与比对）
class R {
    public readonly string $r { get => "x"; }
}
