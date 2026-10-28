# Installs Video Wall Control as a Task Scheduler task that starts hidden at logon.
# Run from an elevated PowerShell, or just double-click install.bat.

param(
    [string]$TaskName = "VideoWallControl",
    [int]$DashboardPort = 8080,
    [switch]$NoStart
)

$ErrorActionPreference = "Stop"
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $AppDir "videowall.py"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Error "Please run this script as Administrator (right-click install.bat > Run as administrator)."
}

# --- Locate pythonw.exe (windowless Python) -------------------------------
function Find-PythonW {
    $candidates = @()
    foreach ($launcher in @("py", "python")) {
        $cmd = Get-Command $launcher -ErrorAction SilentlyContinue
        if ($cmd) {
            $pyArgs = if ($launcher -eq "py") { @("-3", "-c") } else { @("-c") }
            try {
                $exe = & $cmd.Source @pyArgs "import sys; print(sys.executable)" 2>$null
                if ($exe) { $candidates += (Join-Path (Split-Path $exe) "pythonw.exe") }
            } catch { }
        }
    }
    $candidates += (Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\pythonw.exe", "C:\Program Files\Python3*\pythonw.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | ForEach-Object FullName)
    foreach ($c in $candidates) {
        if ($c -and (Test-Path $c) -and ($c -notmatch "WindowsApps")) { return $c }
    }
    return $null
}

$PythonW = Find-PythonW
if (-not $PythonW) {
    Write-Error "pythonw.exe not found. Install Python 3 from https://www.python.org/downloads/windows/ (tick 'Add python.exe to PATH') and re-run."
}
Write-Host "Using Python: $PythonW"

# --- Scheduled task --------------------------------------------------------
# Runs in the logged-on user's interactive session: Studio Monitor windows need a desktop,
# so this must NOT be a Session 0 Windows service.
$user = "$env:USERDOMAIN\$env:USERNAME"
$action = New-ScheduledTaskAction -Execute $PythonW -Argument "`"$Script`"" -WorkingDirectory $AppDir
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
    -Description "Video Wall Control: NDI Studio Monitor watchdog + web dashboard" | Out-Null
Write-Host "Scheduled task '$TaskName' registered (runs at logon of $user)."

# --- Firewall rule for the dashboard ---------------------------------------
$ruleName = "Video Wall Control Dashboard ($DashboardPort)"
if (-not (Get-NetFirewallRule -DisplayName $ruleName -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName $ruleName -Direction Inbound -Protocol TCP `
        -LocalPort $DashboardPort -Action Allow -Profile Any | Out-Null
    Write-Host "Firewall rule added for TCP $DashboardPort."
}

if (-not $NoStart) {
    Start-ScheduledTask -TaskName $TaskName
    Write-Host "Started. Dashboard: http://localhost:$DashboardPort (or http://<this-pc-ip>:$DashboardPort)"
}

Write-Host ""
Write-Host "For fully unattended boots, enable automatic Windows sign-in for $env:USERNAME"
Write-Host "(e.g. Sysinternals Autologon: https://learn.microsoft.com/sysinternals/downloads/autologon)."
