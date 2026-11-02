@echo off
:: Double-click to update this PC (media server or receiving PC). Re-launches itself elevated,
:: since restarting the scheduled tasks needs admin rights.
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" & exit /b)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0update.ps1"
pause
