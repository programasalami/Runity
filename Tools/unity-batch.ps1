# Runs Unity in batch mode on the Runity project with a stall watchdog (the editor occasionally stops making progress in batch mode).
#   powershell -File Tools\unity-batch.ps1 -Log TestResults\x.log -Args "-runTests","-testPlatform","EditMode","-testResults","C:\...\x.xml"
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [string[]]$Args = @(),
    [switch]$NoGraphics,
    [int]$StallMinutes = 10,
    [int]$MaxMinutes = 45
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$unity = 'C:\Program Files\Unity\Hub\Editor\6000.6.4f1\Editor\Unity.exe'
$logPath = if ([IO.Path]::IsPathRooted($Log)) { $Log } else { Join-Path $root $Log }
if (Test-Path $logPath) {
    # A killed editor can hold its log for a while: never fail on it, write a fresh file instead.
    try { Remove-Item $logPath -ErrorAction Stop } catch { $logPath = $logPath -replace '\.log$', ('-' + (Get-Date -Format 'HHmmss') + '.log'); Write-Output "log: $logPath" }
}
$all = @('-batchmode', '-projectPath', (Join-Path $root 'Runity'), '-logFile', $logPath) + $Args
if ($NoGraphics) { $all = @('-nographics') + $all }
$p = Start-Process -FilePath $unity -ArgumentList $all -PassThru
$start = Get-Date
$lastSize = -1; $lastChange = Get-Date
while (-not $p.HasExited) {
    Start-Sleep -Seconds 15
    $size = if (Test-Path $logPath) { (Get-Item $logPath).Length } else { 0 }
    if ($size -ne $lastSize) { $lastSize = $size; $lastChange = Get-Date }
    $stalled = ((Get-Date) - $lastChange).TotalMinutes -ge $StallMinutes
    $tooLong = ((Get-Date) - $start).TotalMinutes -ge $MaxMinutes
    if ($stalled -or $tooLong) {
        Write-Output ("watchdog: Unity " + $(if ($stalled) { "made no progress for $StallMinutes minutes" } else { "ran over $MaxMinutes minutes" }) + " - stopping it")
        Get-CimInstance Win32_Process -Filter "Name='Unity.exe'" | Where-Object { $_.CommandLine -like '*Runity*Runity*' } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
        exit 3
    }
}
Write-Output "unity exit $($p.ExitCode) after $([int]((Get-Date) - $start).TotalMinutes) min"
exit $p.ExitCode
