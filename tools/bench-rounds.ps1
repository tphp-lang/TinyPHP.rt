# bench-rounds.ps1 - multi-round bench runner: min/median/spread aggregation
#
# Why: a single harness run is one sample per item; local single-run noise is
# ~±10% (observed array-read 9.7-11.5 across fresh builds). Running the whole
# harness N times in fresh processes and reporting min/median filters OS jitter
# without changing the harness itself (historical single-run baselines stay
# comparable; min-of-N is slightly biased low vs single runs).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\bench-rounds.ps1
#   ... -Rounds 7 -Only kv-read -HighPriority -AffinityMask 15
#
# -Warmup runs are executed but discarded (first process of a fresh build
# pays cold caches / CPU ramp; e.g. array-read 11.5 -> 10.1 after round 1).
# -HighPriority / -AffinityMask are set on this shell (children inherit) and
# restored afterwards; affinity cuts core-migration jitter, use opt-in only.
param(
    [int]$Rounds = 5,
    [int]$Warmup = 1,
    [string]$Only = "",
    [switch]$HighPriority,
    [int]$AffinityMask = 0,
    [string]$Exe = ".\build\tphp.exe"
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

$proc = Get-Process -Id $PID
$oldPrio = $null
$oldAff = $null
try { $oldPrio = $proc.PriorityClass } catch {}
try { $oldAff = $proc.ProcessorAffinity } catch {}

function Restore-Env {
    param($p, $prio, $aff)
    if ($null -ne $prio) { try { $p.PriorityClass = $prio } catch {} }
    if ($null -ne $aff) { try { $p.ProcessorAffinity = $aff } catch {} }
}

try {
    if ($HighPriority) {
        try { $proc.PriorityClass = "High" }
        catch { Write-Warning "cannot raise priority (need perms?): $_" }
    }
    if ($AffinityMask -ne 0) {
        try { $proc.ProcessorAffinity = [IntPtr]$AffinityMask }
        catch { Write-Warning "cannot set affinity: $_" }
    }

    $samples = @{}      # bench name -> array of ms
    $order = @()
    $fails = @()
    $allOk = $false
    $total = $Warmup + $Rounds

    for ($r = 1; $r -le $total; $r++) {
        $keep = $r -gt $Warmup
        $out = (& $Exe run tools\bench-vs-php.php 2>&1 | Out-String) -split "`r?`n"
        foreach ($line in $out) {
            # PS ErrorRecord rendering embeds the message text in its
            # CategoryInfo line -> would double-count (fib showed n=10)
            if ($line -match '^\s*\+\s+(CategoryInfo|FullyQualifiedErrorId|PositionMessage)') { continue }
            if ($line -match 'all-ok') { $allOk = $true }
            if ($line -match '\w+-FAIL') { $fails += $line.Trim() }
            if ($line -match 'BENCH\s+(.+?)\s+([0-9.]+)\s+ms') {
                $name = $Matches[1].Trim()
                $ms = [double]$Matches[2]
                if ($Only -and ($name -notlike "*$Only*")) { continue }
                if ($keep) {
                    if (-not $samples.ContainsKey($name)) {
                        $samples[$name] = @()
                        $order += $name
                    }
                    $samples[$name] += $ms
                }
            }
        }
    }

    if ($fails.Count -gt 0) {
        Write-Warning ("result check FAILED: " + ($fails -join "; "))
    } elseif (-not $allOk) {
        Write-Warning "no 'all-ok' seen - harness may have died mid-run"
    }

    $fmt = "{0,-20} {1,8} {2,8} {3,8} {4,7} {5,5}"
    Write-Host ($fmt -f "item", "min", "med", "max", "spread", "n")
    foreach ($name in $order) {
        $v = ($samples[$name] | Sort-Object)
        $n = $v.Count
        $min = $v[0]
        $max = $v[$n - 1]
        $med = if ($n % 2 -eq 1) { $v[[int](($n - 1) / 2)] }
               else { ($v[$n / 2 - 1] + $v[$n / 2]) / 2 }
        $spread = if ($min -gt 0) { ($max - $min) / $min * 100 } else { 0 }
        Write-Host ($fmt -f $name,
            ("{0:N1}" -f $min), ("{0:N1}" -f $med), ("{0:N1}" -f $max),
            ("{0:N1}%" -f $spread), "$n")
    }
} finally {
    Restore-Env $proc $oldPrio $oldAff
    Pop-Location
}
