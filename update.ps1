# Updates this PC to the latest version: git pull, rebuild the mosaic engine and editor if they
# are built here, and restart whatever was installed (dashboard and/or engine).
# Works on the media server and on receiving PCs. Double-click update.bat.
$ErrorActionPreference = "Stop"
$AppDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$EngineDir = Join-Path $AppDir "mosaic-engine"
$Tasks = @("VideoWallControl", "MosaicEngine") | Where-Object { Get-ScheduledTask -TaskName $_ -ErrorAction SilentlyContinue }

function Step($msg) { Write-Host ""; Write-Host "== $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host $msg -ForegroundColor Red; $script:failed = $true }
$failed = $false

Step "Getting the latest version"
Push-Location $AppDir
git pull --ff-only
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Host "git pull failed (local changes, or no network). Nothing was changed or restarted." -ForegroundColor Red
    exit 1
}
Pop-Location

$engineBuilt = Test-Path (Join-Path $EngineDir "build\CMakeCache.txt")
if ($Tasks) {
    Step "Stopping $($Tasks -join ', ')"
    # Studio Monitor windows keep running, so the screens stay up during the update.
    foreach ($t in $Tasks) { Stop-ScheduledTask -TaskName $t -ErrorAction SilentlyContinue }
}

if ($engineBuilt) {
    # The exe is locked while it runs, so the engine is stopped for the build (a few seconds of no NDI).
    Get-Process mosaic_engine -ErrorAction SilentlyContinue | Stop-Process -Force
    Step "Building the mosaic engine"
    cmake --build (Join-Path $EngineDir "build") --config Release
    if ($LASTEXITCODE -ne 0) { Fail "Engine build failed (see above). The previous engine is started again." }

    Step "Building the editor"
    Push-Location (Join-Path $EngineDir "ui")
    $lock = Get-Item package-lock.json
    $stamp = "node_modules\.package-lock.json"
    if (-not (Test-Path $stamp) -or (Get-Item $stamp).LastWriteTime -lt $lock.LastWriteTime) {
        npm ci --no-audit --no-fund
        if ($LASTEXITCODE -ne 0) { Fail "npm ci failed." }
    }
    npm run build
    if ($LASTEXITCODE -ne 0) { Fail "Editor build failed." }
    Pop-Location
} else {
    Write-Host "(No engine build on this PC; only the dashboard is updated.)"
}

if ($Tasks) {
    Step "Starting $($Tasks -join ', ')"
    foreach ($t in $Tasks) { Start-ScheduledTask -TaskName $t }
} elseif ($engineBuilt) {
    Write-Host "Nothing is installed to start at logon here. Start the engine with:"
    Write-Host "  cd mosaic-engine; build\Release\mosaic_engine.exe"
}

Write-Host ""
$version = git -C $AppDir log -1 --format="%h %s"
if ($failed) { Write-Host "Update finished with errors. Now at: $version" -ForegroundColor Yellow; exit 1 }
Write-Host "Up to date: $version" -ForegroundColor Green
