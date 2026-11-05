# Registers the "MosaicUpdate" scheduled task: runs update.ps1 -Unattended on demand, as this
# user with admin rights. The editor's "Update everything" starts it (schtasks /Run), so the
# update runs outside the engine it stops and rebuilds. Called by install-engine.ps1 and
# update.ps1; needs an elevated PowerShell.
$EngineDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$AppDir = Split-Path -Parent $EngineDir
$user = "$env:USERDOMAIN\$env:USERNAME"
$script = Join-Path $AppDir "update.ps1"
$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`" -Unattended" -WorkingDirectory $AppDir
$taskPrincipal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 30) -MultipleInstances IgnoreNew
if (Get-ScheduledTask -TaskName "MosaicUpdate" -ErrorAction SilentlyContinue) {
    Unregister-ScheduledTask -TaskName "MosaicUpdate" -Confirm:$false
}
Register-ScheduledTask -TaskName "MosaicUpdate" -Action $action -Principal $taskPrincipal -Settings $settings `
    -Description "Updates video-wall-control (git pull, rebuild, restart); started from the mosaic engine's editor" | Out-Null
Write-Host "Scheduled task 'MosaicUpdate' registered (the editor's Update button runs it)."
