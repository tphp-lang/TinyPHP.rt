# convert-phpt.ps1 - convert php-src .phpt (--EXPECT-- only) to tphp test-*.php
# Usage: powershell -File convert-phpt.ps1 -Src <zend/tests dir> -Dst <tests/php-compat> -Limit 50
param(
    [string]$Src = "C:\Users\28249\Desktop\php-src-php-8.5.11\Zend\tests",
    [string]$Dst = "tests\php-compat",
    [int]$Limit = 50
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Path $Dst -Force | Out-Null

# only take small files with pure --EXPECT-- (not EXPECTF/EXPECTREGEX)
$files = Get-ChildItem $Src -Recurse -Filter "*.phpt" |
    Where-Object { $_.Length -lt 1500 } |
    Sort-Object Name

$converted = 0
foreach ($f in $files) {
    if ($converted -ge $Limit) { break }
    $raw = Get-Content $f.FullName -Raw
    # section split
    if ($raw -notmatch '(?s)--FILE--\s*\n(.*?)\n--EXPECT--\s*\n(.*)$') { continue }
    # reject EXPECTF / EXPECTREGEX / EXPECTHEADERS
    if ($raw -match '--EXPECTF--|--EXPECTREGEX--|--EXPECTHEADERS--|--SKIPIF--') { continue }
    $code = $Matches[1]
    $expect = $Matches[2]
    # strip trailing section markers
    $expect = $expect -replace '(?s)\n--.*$', ''
    # must look like php
    if ($code -notmatch '^\s*<\?php') { $continue = $true; continue }

    # name: sanitize — include parent dir to avoid collisions
    $parent = $f.Directory.Name
    $base = ($f.BaseName -replace '[^A-Za-z0-9_]', '_')
    $pname = ($parent -replace '[^A-Za-z0-9_]', '_')
    $name = "test-$pname-$base.php"
    if ($name.Length -gt 80) { $name = "test-" + $f.BaseName.Substring(0, [Math]::Min(50, $f.BaseName.Length)) + ".php" }
    $outPath = Join-Path $Dst $name
    if (Test-Path $outPath) { continue }

    # build output file: header comments + code with EXPECT lines prepended
    $expLines = @()
    foreach ($line in ($expect -split "`n")) {
        $t = $line.TrimEnd("`r")
        # skip empty trailing lines (tphp runner does not emit them)
        if ($t -eq '') { continue }
        $expLines += "// EXPECT: $t"
    }
    if ($expLines.Count -eq 0) { continue }
    # code: ensure starts with <?php
    $body = $code.Trim()
    # drop leading <?php if we prepend our own
    $body = $body -replace '^<\?php\s*', ''

    $content = "<?php`n"
    $content += "// from php-src Zend/tests/$($f.Name)`n"
    foreach ($e in $expLines) { $content += "$e`n" }
    $body = $code.Trim()
    $body = $body -replace '^<\?php\s*', ''
    $body = $body -replace '\?>\s*$', ''
    $content += $body
    if (-not $content.EndsWith("`n")) { $content += "`n" }

    [IO.File]::WriteAllText($outPath, $content, [Text.UTF8Encoding]::new($false))
    $converted++
    Write-Host "converted: $name"
}
Write-Host "done: $converted file(s) -> $Dst"
