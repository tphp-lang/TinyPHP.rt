<?php
// namespace declarations, use imports, name resolution, ::class
// EXPECT: in-ns: woof
// EXPECT: App\Model\Dog [Rex]
// EXPECT: fallback-strlen: 3
// EXPECT: usefn: NS-fn
// EXPECT: usefn-alias: NS-fn
// EXPECT: usefn: NS-fn
// EXPECT: useconst: NS-PI
// EXPECT: useconst-alias: NS-PI
// EXPECT: local-const: NS-E
// EXPECT: abs: NS-C
// EXPECT: imported: App\Lib\Helper
// EXPECT: alias-isa: yes
// EXPECT: App\Model\Dog [Rex2]
// EXPECT: App\Model\Status
// EXPECT: App\Model\Status
// EXPECT: self-class: App\Other\Status2
// EXPECT: fq-call: OTHER-fn
// EXPECT: fallback-quad: 16
// EXPECT: global-quad: 9
// EXPECT: global-const
// EXPECT: App\Other\Tool
// EXPECT: after-ns-end

namespace App\Model;

function ns_len($s) { return "NS-fn"; }

const NS_PI = "NS-PI";

class Dog {
    public $name;
    public function __construct($n) { $this->name = $n; }
    public function speak() { return "woof"; }
    public static function describe($d) {
        return static::class . " [{$d->name}]";
    }
}

enum Status: string {
    case Active = "A";
    case Closed = "C";
}

// unqualified class name inside the namespace
$d = new Dog("Rex");
echo "in-ns: ", $d->speak(), "\n";
echo Dog::describe($d), "\n";

// unqualified functions/constants fall back to the global scope
echo "fallback-strlen: ", strlen("abc"), "\n";

// own-namespace function resolves first
echo "usefn: ", ns_len("x"), "\n";

namespace App\Lib;

class Helper {
    public function who() { return get_class($this); }
}

namespace App\Other;

use function App\Model\ns_len as mlen;
use function App\Model\ns_len;
use const App\Model\NS_PI;
use const App\Model\NS_PI as PPI;
use App\Lib\Helper;
use App\Model\Dog as K9;
use App\Model\Status as St;

function ns_len($s) { return "OTHER-fn"; }
const NS_PI = "OTHER-PI";
const NS_E = "NS-E";

class Status2 {
    public function cls() { return self::class; }
}

// use function beats the namespace lookup; same-alias local loses
echo "usefn-alias: ", mlen("y"), "\n";
echo "usefn: ", ns_len("q"), "\n";
echo "useconst: ", NS_PI, "\n";
echo "useconst-alias: ", PPI, "\n";
echo "local-const: ", NS_E, "\n";

// absolute names skip imports
$e = new \Exception("NS-C");
echo "abs: ", $e->getMessage(), "\n";

// imported class alias
echo "imported: ", (new Helper())->who(), "\n";
// imported alias used in a static call + instanceof
$k = new K9("Rex2");
echo "alias-isa: ", $k instanceof Helper ? "no" : "yes", "\n";
echo K9::describe($k), "\n";

// ::class forms
echo St::Active::class, "\n";
echo \App\Model\Status::class, "\n";
echo "self-class: ", (new Status2())->cls(), "\n";

// fully qualified own-namespace call
echo "fq-call: ", \App\Other\ns_len("z"), "\n";

// unqualified call falls back to the global scope (hoisted)
echo "fallback-quad: ", quad(4), "\n";

// the global block may reference namespaced classes by FQ name
namespace {
    function quad($x) { return $x * $x; }
    define("G_K", "global-const");
    echo "global-quad: ", quad(3), "\n";
    echo G_K, "\n";
    $t = new \App\Other\Tool();
    echo get_class($t), "\n";
    echo "after-ns-end", "\n";
}

namespace App\Other {
    class Tool {}
}
