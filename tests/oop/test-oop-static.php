<?php
// 静态：静态属性/方法、self/static/parent、静态计数器
// EXPECT: next: 1, 2, 3
// EXPECT: shared: 3
// EXPECT: self-name: App\Support\Registry
// EXPECT: static-name: App\Support\Registry
// EXPECT: derived-ctor: D-init; A-init

namespace App\Support;

class Registry {
    public static $count = 0;
    public static function next() {
        self::$count++;
        return self::$count;
    }
    public function who() { return self::class; }
    public function dynamicStatic() { return static::class; }
}

echo "next: ", Registry::next(), ", ", Registry::next(), ", ", Registry::next(), "\n";
$r = new Registry();
echo "shared: ", Registry::$count, "\n";
echo "self-name: ", $r->who(), "\n";
echo "static-name: ", $r->dynamicStatic(), "\n";

class BaseJob {
    public static $log = [];
    public function __construct() {
        static::$log[] = "A-init";
    }
}

class DerivedJob extends BaseJob {
    public function __construct() {
        echo "derived-ctor: D-init; ";
        parent::__construct();
    }
}

$d = new DerivedJob();
$last = DerivedJob::$log[count(DerivedJob::$log) - 1];
echo $last, "\n";
