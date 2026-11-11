@echo off
:: Double-click to set up this PC as a Mosaic Receiver (installs everything, builds, starts).
:: Asks for the media server's name. Re-launches itself elevated if needed.
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs" & exit /b)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-receiver.ps1" %*
pause
