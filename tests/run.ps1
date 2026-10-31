# TinyPHP.rt test runner
# Tests: tests/*/test-*.php with txiki-style `// EXPECT:` comments.
# Each EXPECT line is compared (exact) against the script's stdout lines.
# Cross-platform: Windows (tphp.exe) and Unix (tphp) via pwsh/powershell.
$ErrorActionPreference = "Continue"
# Child stdout is raw UTF-8 (tphp sets CP_UTF8 on Windows); without pinning
# the decode here, a 936-CP console turns Chinese EXPECT lines into mojibake
# non-deterministically (console CP is process-shared state).
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }
$root = Split-Path -Parent $PSScriptRoot
$onWindows = ($env:OS -eq 'Windows_NT') -or ($IsWindows -eq $true)
if ($onWindows) {
    $tphp = Join-Path $root "build\tphp.exe"
} else {
    $tphp = Join-Path $root "build/tphp"
}
if (-not (Test-Path $tphp)) {
    # cross-name fallback
    $alt = if ($onWindows) { Join-Path $root "build/tphp" } else { Join-Path $root "build\tphp.exe" }
    if (Test-Path $alt) { $tphp = $alt }
    else {
        Write-Host "tphp binary not found - run build.cmd / build.sh first"
        exit 1
    }
}

# Windows-only tests (msvcrt/user32/kernel32/lstrcmpW) — skipped on Linux/macOS
$winOnly = @(
    "test-ffi-simple-calls.php",
    "test-ffi-system-libs.php",
    "test-ffi-variadic.php",
    "test-ffi-wchar.php",
    "test-ffi-in-worker.php",
    "m5.5-acceptance.php"
)

$pass = 0
$fail = 0
$skip = 0
$failed = @()

foreach ($dir in @("lang", "oop", "async", "worker", "ffi", "jit", "lib", "pkg", "bench", "php-compat", "standard")) {
    $dirPath = Join-Path $PSScriptRoot $dir
    if (-not (Test-Path $dirPath)) { continue }
    $files = Get-ChildItem -Path $dirPath -Filter "*.php" -ErrorAction SilentlyContinue |
        Sort-Object Name
    # only test-*.php (helpers/non-tests excluded)
    $files = $files | Where-Object { $_.Name -like "test-*.php" }
    foreach ($_ in $files) {
        if (-not $onWindows -and $winOnly -contains $_.Name) {
            $skip++
            Write-Host "SKIP $($_.Name) (windows-only)"
            continue
        }
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
            # crash diagnostics: separate compile vs execute
            if ($exit -ne 0 -and $exit -ne $wantExit) {
                $dumpOk = $true
                $dumpOut = @(& $tphp --dump-opcodes $_.FullName 2>&1 | ForEach-Object { "$_" })
                $dumpExit = $LASTEXITCODE
                if ($dumpExit -ne 0) {
                    $dumpOk = $false
                    Write-Host "  DIAG compile --dump-opcodes exit=$dumpExit"
                    $dumpOut | Select-Object -First 10 | ForEach-Object { Write-Host "  D: $_" }
                } else {
                    Write-Host "  DIAG compile OK (opcodes dumped); crash is at execute"
                }
                # rerun with stderr visible
                $errOut = @(& $tphp run $_.FullName 2>&1 | ForEach-Object { "$_" })
                if ($errOut) {
                    Write-Host "  DIAG run stderr/out:"
                    $errOut | Select-Object -First 10 | ForEach-Object { Write-Host "  R: $_" }
                }
            }
        }
    }
}

Write-Host ""
if ($skip -gt 0) { Write-Host "$pass passed, $fail failed, $skip skipped" }
else { Write-Host "$pass passed, $fail failed" }
if ($fail -gt 0) { exit 1 }
exit 0
