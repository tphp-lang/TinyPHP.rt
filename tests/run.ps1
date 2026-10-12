# TinyPHP.rt test runner
# Tests: tests/lang/test-*.php and tests/oop/*.php with txiki-style `// EXPECT:` comments.
# Each EXPECT line is compared (exact) against the script's stdout lines.
$ErrorActionPreference = "Continue"
$tphp = Join-Path $PSScriptRoot "..\build\tphp.exe"
if (-not (Test-Path $tphp)) {
    Write-Host "tphp.exe not found at $tphp - run build.cmd first"
    exit 1
}

$pass = 0
$fail = 0
$failed = @()

foreach ($dir in @("lang", "oop", "async", "worker")) {
$files = Get-ChildItem -Path (Join-Path $PSScriptRoot $dir) -Filter "*.php" |
    Sort-Object Name
if ($dir -eq "lang") {
    $files = $files | Where-Object { $_.Name -like "test-*.php" }
}
foreach ($_ in $files) {
        $expected = @()
        $wantExit = 0
        foreach ($line in Get-Content -Encoding UTF8 $_.FullName) {
            if ($line -match '^\s*//\s*EXPECT:\s?(.*)$') { $expected += $Matches[1] }
            if ($line -match '^\s*//\s*EXPECT-EXIT:\s?(\d+)') { $wantExit = [int]$Matches[1] }
        }
        $output = @(& $tphp run $_.FullName 2>$null | ForEach-Object { "$_" })
        $exit = $LASTEXITCODE
        $ok = ($exit -eq $wantExit)
        if ($ok -and ($output.Count -ne $expected.Count)) { $ok = $false }
        if ($ok) {
            for ($i = 0; $i -lt $expected.Count; $i++) {
                if ($output[$i] -cne $expected[$i]) { $ok = $false; break }
            }
        }
        if ($ok) {
            $pass++
            Write-Host "PASS $($_.Name)"
        } else {
            $fail++
            Write-Host "FAIL $($_.Name) (exit=$exit)"
            $failed += $_.Name
            $n = [Math]::Max($output.Count, $expected.Count)
            for ($i = 0; $i -lt $n; $i++) {
                $e = if ($i -lt $expected.Count) { $expected[$i] } else { "<none>" }
                $g = if ($i -lt $output.Count) { $output[$i] } else { "<none>" }
                if ($e -cne $g) {
                    Write-Host ("  line {0}: expected [{1}] got [{2}]" -f ($i + 1), $e, $g)
                }
            }
        }
    }
}

Write-Host ""
Write-Host "$pass passed, $fail failed"
if ($fail -gt 0) { exit 1 }
exit 0
