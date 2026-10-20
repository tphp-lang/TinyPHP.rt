<?php
// EXPECT: files-ok
// EXPECT: mod=foo/bar 0.1.0 lib MIT
// EXPECT: smoke=hello from Foo\Bar
// EXPECT: cleanup-ok

// pkg lib scaffold test: uses build/ as scratch (gitignored)
$root = __DIR__ . "/../..";
$dir = $root . "/build/pkgtest-lib";

function pkg_rm_tree($d) {
    $entries = scandir($d);
    if ($entries === false) {
        unlink($d);
        return;
    }
    foreach ($entries as $f) {
        if ($f === "." || $f === "..") continue;
        pkg_rm_tree($d . "/" . $f);
    }
    rmdir($d);
}

if (file_exists($dir)) pkg_rm_tree($dir);

pkg_new($dir, "foo/bar", true);

$need = ["mod.json", "src/Greeter.php", "tests/test-smoke.php",
         "README.md", ".gitignore", "vendor/autoload.php"];
$allok = true;
foreach ($need as $f) {
    if (!file_exists($dir . "/" . $f)) {
        echo "missing=", $f, "\n";
        $allok = false;
    }
}
if ($allok) echo "files-ok\n";

$m = json_decode(file_get_contents($dir . "/mod.json"));
echo "mod=", $m["name"], " ", $m["version"], " ", $m["type"], " ", $m["license"], "\n";

// psr-4 autoload end-to-end: use + first reference triggers the loader
require $dir . "/vendor/autoload.php";
use Foo\Bar\Greeter;
echo "smoke=", Greeter::hello(), "\n";

// generated smoke test itself passes expectations
// (run its EXPECT via require: echoes smoke=... once more would double;
//  instead just confirm the file loads and the class resolves)

pkg_rm_tree($dir);
echo "cleanup-ok\n";
