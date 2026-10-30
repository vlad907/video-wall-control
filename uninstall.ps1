# Removes the Video Wall Control scheduled task and firewall rule.
param(
    [string]$TaskName = "VideoWallControl",
    [int]$DashboardPort = 8080
)

if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
    Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "Removed scheduled task '$TaskName'."
}

# Stop a running copy that may have been started manually.
Get-CimInstance Win32_Process -Filter "Name = 'pythonw.exe' OR Name = 'python.exe'" |
    Where-Object { $_.CommandLine -like "*videowall.py*" } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force; Write-Host "Stopped PID $($_.ProcessId)." }

Get-NetFirewallRule -DisplayName "Video Wall Control Dashboard ($DashboardPort)" -ErrorAction SilentlyContinue |
    Remove-NetFirewallRule
Get-NetFirewallRule -DisplayName "Mosaic Engine Editor (8090)", "Mosaic Engine NDI" -ErrorAction SilentlyContinue |
    Remove-NetFirewallRule
Write-Host "Done. Studio Monitor instances and the mosaic engine were left running."
