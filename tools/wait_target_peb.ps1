# Block until r5apex_dx12 exists, then (optional) settle, then run get_target_peb.ps1.
# Same stdout as get_target_peb.ps1 (PID + PEB lines). Exit 0 on success, 1 on timeout.
param(
    [int]$TimeoutSec = 180,
    [int]$PollSec = 2,
    [int]$SettleSec = 5
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$pebPs1 = Join-Path $root 'get_target_peb.ps1'
if (-not (Test-Path $pebPs1)) { throw "missing $pebPs1" }

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$settled = $false
while ((Get-Date) -lt $deadline) {
    $p = Get-Process -Name r5apex_dx12 -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        if (-not $settled) {
            Start-Sleep -Seconds $SettleSec
            $settled = $true
        }
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = 'powershell.exe'
        $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$pebPs1`""
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $proc = [System.Diagnostics.Process]::Start($psi)
        $out = $proc.StandardOutput.ReadToEnd()
        $proc.WaitForExit()
        if ($proc.ExitCode -eq 0) {
            Write-Output $out.TrimEnd()
            exit 0
        }
    } else {
        $settled = $false
    }
    Start-Sleep -Seconds $PollSec
}
Write-Error "wait_target_peb: r5apex_dx12 / PEB not ready within ${TimeoutSec}s"
exit 1
