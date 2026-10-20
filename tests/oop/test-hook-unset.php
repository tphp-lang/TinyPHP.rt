<?php
// EXPECT: Cannot unset hooked property H::$p
// EXPECT: ok

// 任一钩子存在 → unset 禁止
class H {
    public string $p = "a" { get => $this->p; }
}
try {
    $h = new H();
    unset($h->p);
} catch (Error $e) {
    echo $e->getMessage(), "\n";
}
echo "ok\n";
