<?php
// EXPECT: self-read-ok
// EXPECT: subclass-static-ok
// EXPECT: subclass-instance-ok
// EXPECT: external-still-denied

// 可见性 scope = 方法声明类（PHP EG(scope)）：父类方法内的 self::/this
// 私有访问在经子类调用时同样合法（8.5.1 对拍 B::vs() 读父 private static
// 返回 42）。帧 vis_scope 优先取 TMethod.decl（trait=使用类），闭包帧
// bindTo 走 called_cls fallback——见 bindto-scope/closure 测试。
class SA {
    private static ?int $sx = null;
    private int $sy = 7;
    public static function vs(): int {
        if (self::$sx === null) self::$sx = 42;   // self 读/写 private static
        return self::$sx;
    }
    public function gi(): int { return $this->sy; }
}
class SB extends SA {}

// 1) 直接父类调用
echo SA::vs() === 42 ? "self-read-ok\n" : "self-read-FAIL\n";
// 2) 子类触发父类静态方法（原 bug：called_cls=SB 拒绝）
echo SB::vs() === 42 ? "subclass-static-ok\n" : "subclass-static-FAIL\n";
// 3) 子类实例调用父类实例方法
echo (new SB())->gi() === 7 ? "subclass-instance-ok\n" : "subclass-instance-FAIL\n";
// 4) 外部直读子类私有仍拒绝（PHP 同）
$denied = false;
try { SB::$sx; } catch (Error $e) {
    $denied = $e->getMessage() === "Cannot access private property SA::\$sx";
}
echo $denied ? "external-still-denied\n" : "external-FAIL\n";
