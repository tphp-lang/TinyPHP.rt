# TinyPHP.rt - cross-runtime performance comparison
# tphp (strongest available state; IR JIT on x86_64) vs native PHP strongest:
# opcache.enable_cli=1 + opcache.jit=tracing + jit_buffer. Pure-compute
# benches only (tools/bench-vs-php.php has no async/worker items - PHP
# native has neither).
#
# Usage:
#   pwsh tools/perf-compare.ps1                 # auto-detect tphp, report only
#   pwsh tools/perf-compare.ps1 -MaxFibRatio 6  # fail if fib(28) ratio > 6
#
# Portable: Windows PowerShell 5.1 and pwsh (Linux/macOS runners).
param(
    [string]$Tphp = "",
    [string]$Php = "php",
    [string]$Script = "",
    [double]$MaxFibRatio = 0,   # 0 = report-only; >0 = gate on fib(28)
    [string]$Report = ""
)
$ErrorActionPreference = "Continue"

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Tphp) {
    foreach ($c in @("build/tphp.exe", "build/tphp")) {
        $p = Join-Path $root $c
        if (Test-Path $p) { $Tphp = $p; break }
    }
}
if (-not $Tphp -or -not (Test-Path $Tphp)) {
    Write-Host "ERROR: tphp binary not found (build/tphp[.exe])"
    exit 1
}
if (-not $Script) { $Script = Join-Path $root (Join-Path "tools" "bench-vs-php.php") }
if (-not (Test-Path $Script)) {
    Write-Host "ERROR: bench script not found: $Script"
    exit 1
}

# strongest PHP: opcache for CLI + tracing JIT + buffer
$phpStrong = @(
    "-d", "opcache.enable=1",
    "-d", "opcache.enable_cli=1",
    "-d", "opcache.jit=tracing",
    "-d", "opcache.jit_buffer_size=128M"
)

# --- probe: tracing JIT must actually be live on PHP side ---
# (probe goes through a temp file: PS 5.1 mangles embedded double quotes
# when passing -r code to native exes)
$probeFile = [IO.Path]::GetTempFileName() + ".php"
Set-Content -Path $probeFile -Encoding UTF8 -Value @'
<?php
echo (function_exists('opcache_get_status') && !empty(opcache_get_status(false)['jit']['enabled']))
    ? 'JIT-ON' : 'JIT-OFF';
'@
$probe = & $Php @phpStrong $probeFile 2>$null
Remove-Item $probeFile -ErrorAction SilentlyContinue
if ("$probe" -notmatch "JIT-ON") {
    Write-Host "ERROR: PHP tracing JIT is not active (probe said: $probe)"
    Write-Host "       install/enable opcache; pass -d opcache.enable_cli=1 etc."
    exit 1
}

function Invoke-Bench {
    param([string]$exe, [string[]]$arglist)
    $log = [IO.Path]::GetTempFileName()
    $logOut = [IO.Path]::GetTempFileName()
    $p = Start-Process -FilePath $exe -ArgumentList $arglist -Wait -PassThru `
         -RedirectStandardError $log -RedirectStandardOutput $logOut -NoNewWindow
    $rows = @{}
    Get-Content $log -ErrorAction SilentlyContinue | ForEach-Object {
        # bench labels may contain spaces/exponents ("kv-read 1.6e6");
        # the timing is the last number before "ms"
        if ($_ -match '^BENCH\s+(\S.*?)\s+([\d.]+)\s+ms\s*$') {
            $rows[$Matches[1].Trim()] = [double]$Matches[2]
        }
    }
    Remove-Item $log, $logOut -ErrorAction SilentlyContinue
    @{ rows = $rows; exit = $p.ExitCode }
}

Write-Host "tphp : $Tphp"
Write-Host ("       " + (& $Tphp -v 2>&1 | Select-Object -First 1))
Write-Host "php  : $Php"
Write-Host ("       " + (& $Php -v 2>&1 | Select-Object -First 1))
Write-Host "       strongest: opcache.enable_cli=1 + opcache.jit=tracing + 128M buffer"
Write-Host ""

$rT = Invoke-Bench $Tphp @($Script)
$rP = Invoke-Bench $Php ($phpStrong + @($Script))
if ($rT.exit -ne 0) { Write-Host "WARNING: tphp bench exit=$($rT.exit)" }
if ($rP.exit -ne 0) { Write-Host "WARNING: php bench exit=$($rP.exit)" }

$names = @($rT.rows.Keys) + @($rP.rows.Keys) | Sort-Object -Unique
$lines = @()
$lines += "| bench | tphp | PHP strongest | tphp / PHP |"
$lines += "|---|---:|---:|---:|"
$bad = 0
$fibRatio = 0.0
foreach ($n in $names) {
    $tv = $null; $pv = $null
    if ($rT.rows.ContainsKey($n)) { $tv = $rT.rows[$n] }
    if ($rP.rows.ContainsKey($n)) { $pv = $rP.rows[$n] }
    $ratio = "-"
    if ($tv -ne $null -and $pv -ne $null -and $pv -gt 0) {
        $r = [Math]::Round($tv / $pv, 2)
        $ratio = "{0}x" -f $r
        if ($n -eq "fib(28)") { $fibRatio = $tv / $pv }
    }
    $tvs = if ($tv -ne $null) { "{0}" -f $tv } else { "-" }
    $pvs = if ($pv -ne $null) { "{0}" -f $pv } else { "-" }
    $lines += "| $n | $tvs | $pvs | $ratio |"
}
$table = $lines -join "`n"
Write-Host $table
Write-Host ""

$footer = @()
$footer += ""
if ($fibRatio -gt 0) {
    $footer += ("fib(28) ratio vs PHP strongest: {0:F2}x" -f $fibRatio)
}
$footer += ("tphp bench exit={0}, php bench exit={1}" -f $rT.exit, $rP.exit)
$fj = $footer -join "`n"
Write-Host $fj

if ($Report) {
    $md = @()
    $md += "# tphp vs PHP strongest (opcache + tracing JIT)"
    $md += ""
    $md += ("- tphp: ``$Tphp``")
    $md += ("- php : ``$Php`` (opcache.enable_cli=1, opcache.jit=tracing, buffer=128M)")
    $md += ""
    $md += $table
    $md += ""
    $md += $fj
    $md += ""
    Set-Content -Path $Report -Value ($md -join "`n") -Encoding UTF8
    Write-Host "report written: $Report"
}
if ($env:GITHUB_STEP_SUMMARY) {
    Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value ("## tphp vs PHP strongest`n`n" + $table + "`n" + $fj)
}

$fail = 0
if ($rT.exit -ne 0 -or $rP.exit -ne 0) { $fail = 1 }
if ($MaxFibRatio -gt 0 -and $fibRatio -gt $MaxFibRatio) {
    Write-Host ("ERROR: fib(28) ratio {0:F2}x exceeds gate {1}x" -f $fibRatio, $MaxFibRatio)
    $fail = 1
}
if ($fail) { exit 1 }
Write-Host "perf-compare: OK"
exit 0
