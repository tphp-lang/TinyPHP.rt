<?php
// EXPECT: files-ok
// EXPECT: mod=demo-app 0.1.0 app MIT
// EXPECT: Hello, world
// EXPECT: rerun-deny-ok
// EXPECT: badname-deny-ok
// EXPECT: cleanup-ok

// pkg scaffold test: uses build/ as scratch (gitignored)
$root = __DIR__ . "/../..";
$dir = $root . "/build/pkgtest-app";

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

// clean leftovers from a previous failed run
if (file_exists($dir)) pkg_rm_tree($dir);

pkg_new($dir, "demo-app");

// expected skeleton files
$need = ["mod.json", "main.php", "README.md", ".gitignore", "vendor/autoload.php"];
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

// generated app entry runs standalone
require $dir . "/main.php";

// second new into the same dir must fail
$denied = false;
try {
    pkg_new($dir, "demo-app");
} catch (RuntimeException $e) {
    $denied = true;
}
echo $denied ? "rerun-deny-ok\n" : "rerun-FAIL\n";

// invalid name must raise ValueError
$bad = false;
try {
    pkg_new($root . "/build/pkgtest-bad", "Demo_App");
} catch (ValueError $e) {
    $bad = true;
}
echo $bad ? "badname-deny-ok\n" : "badname-FAIL\n";

pkg_rm_tree($dir);
echo "cleanup-ok\n";
