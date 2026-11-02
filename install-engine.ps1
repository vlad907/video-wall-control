# Media server: starts the mosaic engine at logon, without the video-wall dashboard,
# and keeps it running (run-engine.ps1). Run from an elevated PowerShell, or double-click
# install-engine.bat. -Uninstall removes the task and firewall rules again.
param(
    [string]$TaskName = "MosaicEngine",
    [int]$Port = 8090,
    [string]$Layout = "",
    [switch]$Uninstall,
    [switch]$NoStart
)

$ErrorActionPreference = "Stop"
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$EngineDir = Join-Path $AppDir "mosaic-engine"
$Runner = Join-Path $EngineDir "run-engine.ps1"
$Exe = Join-Path $EngineDir "build\Release\mosaic_engine.exe"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Please run this as Administrator (right-click install-engine.bat > Run as administrator)."
}

if ($Uninstall) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "Removed scheduled task '$TaskName'."
    }
    Get-Process mosaic_engine -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-NetFirewallRule -DisplayName "Mosaic Engine Editor ($Port)", "Mosaic Engine NDI" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    Write-Host "Done. The engine is stopped and no longer starts at logon."
    return
}

if (-not (Test-Path $Exe)) {
    Write-Warning "The engine isn't built yet ($Exe). The task is installed anyway and starts it once it is."
}

# The dashboard can also run the engine. Two engines can't share port $Port, so if the dashboard
# is installed on this PC, it stops running its own copy (it keeps managing Studio Monitors).
$DashConfig = Join-Path $AppDir "config.json"
if ((Get-ScheduledTask -TaskName "VideoWallControl" -ErrorAction SilentlyContinue) -and (Test-Path $DashConfig)) {
    $cfg = Get-Content $DashConfig -Raw | ConvertFrom-Json
    if (-not $cfg.mosaic) { $cfg | Add-Member -NotePropertyName mosaic -NotePropertyValue ([pscustomobject]@{}) }
    if ($cfg.mosaic.enabled -ne $false) {
        $cfg.mosaic | Add-Member -Force -NotePropertyName enabled -NotePropertyValue $false
        $cfg | ConvertTo-Json -Depth 20 | Set-Content -Path $DashConfig -Encoding UTF8
        Write-Host "The dashboard on this PC no longer starts its own engine ('Run engine' turned off)."
        Stop-ScheduledTask -TaskName "VideoWallControl" -ErrorAction SilentlyContinue
        Start-ScheduledTask -TaskName "VideoWallControl"
    }
}

# --- Scheduled task: hidden PowerShell running the watchdog loop ------------
# At logon of this user (not a Session 0 service), so the GPU and NDI behave as when run by hand.
$user = "$env:USERDOMAIN\$env:USERNAME"
$runnerArgs = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$Runner`" -Port $Port"
if ($Layout) { $runnerArgs += " -Layout `"$Layout`"" }
$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $runnerArgs -WorkingDirectory $EngineDir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$taskPrincipal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
    -MultipleInstances IgnoreNew -StartWhenAvailable

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
}
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Principal $taskPrincipal -Settings $settings `
    -Description "Mosaic engine: renders the canvas and sends the NDI outputs (restarted if it stops)" | Out-Null
Write-Host "Scheduled task '$TaskName' registered (runs at logon of $user)."

# --- Firewall: the editor / API port, and NDI (dynamic ports, so a program rule) ---
$editorRule = "Mosaic Engine Editor ($Port)"
if (-not (Get-NetFirewallRule -DisplayName $editorRule -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $editorRule -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow -Profile Any | Out-Null
    Write-Host "Firewall rule added for TCP $Port."
}
Get-NetFirewallRule -DisplayName "Mosaic Engine NDI" -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName "Mosaic Engine NDI" -Direction Inbound -Program $Exe -Action Allow -Profile Any | Out-Null
Write-Host "Firewall rule added for the engine's NDI streams."
Write-Host "If antivirus with its own firewall is installed (ESET, for example), allow port $Port and mosaic_engine.exe there too."

if (-not $NoStart) {
    Get-Process mosaic_engine -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-ScheduledTask -TaskName $TaskName
    Write-Host "Started. Editor: http://localhost:$Port (or http://<this-pc-ip>:$Port)"
}
Write-Host "Logs: $AppDir\logs\mosaic-engine.log and mosaic-engine-watchdog.log"
Write-Host ""
Write-Host "For fully unattended boots, enable automatic Windows sign-in for $env:USERNAME"
Write-Host "(e.g. Sysinternals Autologon: https://learn.microsoft.com/sysinternals/downloads/autologon)."
