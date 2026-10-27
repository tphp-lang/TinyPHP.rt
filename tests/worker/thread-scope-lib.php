<?php
// P1-10 回归辅助：inc_progs（require 进来的文件）里的全局函数与静态方法
// 必须可作 Thread 入口——PHP 单请求全局符号表的等价物。

function thread_scope_worker(): int {
    // 跨 Program 调用：lib → main（入口在 lib，子线程必须能解析主脚本函数）
    return thread_scope_main_part() + 40;
}

function thread_scope_lib_helper(): string {
    return 'L';
}

class ThreadScopeCls {
    public static function stat(): int {
        return 77;
    }

    public function inst(): int {
        return 1;   // 非静态：入口应拒绝
    }
}
