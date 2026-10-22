# mk-cacert.ps1: convert build/cacert.pem into ext/net/cacert.c (embedded CA bundle)
# Usage: powershell -File scripts/mk-cacert.ps1
# (ASCII comments only: PS5 reads BOM-less UTF-8 scripts as ANSI and a CJK
#  comment can swallow the newline, hiding the next line.)
$pem = Join-Path $PSScriptRoot "..\build\cacert.pem"
$out = Join-Path $PSScriptRoot "..\ext\net\cacert.c"
$bytes = [System.IO.File]::ReadAllBytes($pem)
$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("/* 自动生成：scripts/mk-cacert.ps1 <- build/cacert.pem (curl.se/ca)")
[void]$sb.AppendLine(" * Mozilla CA bundle，单 exe 零依赖的 HTTPS 证书链。 */")
[void]$sb.AppendLine("#include <stddef.h>")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("const unsigned char tphp_cacert_pem[] = {")
$line = New-Object System.Text.StringBuilder
for ($i = 0; $i -lt $bytes.Length; $i++) {
    [void]$line.Append(('{0}, ' -f $bytes[$i]))
    if (($i + 1) % 20 -eq 0) {
        [void]$sb.AppendLine(("    " + $line.ToString().TrimEnd()))
        [void]$line.Clear()
    }
}
if ($line.Length -gt 0) { [void]$sb.AppendLine(("    " + $line.ToString().TrimEnd())) }
# mbedtls x509_crt_parse requires a NUL-terminated buffer for PEM detection
[void]$sb.AppendLine("    0x00, 0x00")
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("const size_t tphp_cacert_pem_len = sizeof tphp_cacert_pem - 1;")
[System.IO.File]::WriteAllText($out, $sb.ToString())
Write-Host ("wrote {0} ({1} bytes pem)" -f $out, $bytes.Length)
