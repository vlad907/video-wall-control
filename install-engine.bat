@echo off
:: Media server: start the mosaic engine at logon and keep it running. Re-launches itself elevated if needed.
:: "install-engine.bat -Uninstall" removes it again.
net session >nul 2>&1 && goto :run
if "%~1"=="" (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs") else (powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%*' -Verb RunAs")
exit /b
:run
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-engine.ps1" %*
pause
