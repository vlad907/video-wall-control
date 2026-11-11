# Receiving PC, from nothing to a running Mosaic Receiver, unattended:
#   1. installs Git, Python 3, CMake, Visual Studio 2022 Build Tools (C++), the NDI 6 SDK
#      and NDI Tools (Studio Monitor, the fallback player)
#   2. clones video-wall-control (or pulls the latest)
#   3. builds mosaic_receiver.exe (Release) and runs its self-test
#   4. points the dashboard at the media server running the engine
#   5. installs the dashboard (starts at logon, firewall rules) and starts it
#   6. registers this PC with the engine as an endpoint with player "Mosaic Receiver"
#
# One command, in PowerShell (as Administrator avoids the UAC prompt), on a bare PC:
#   Set-ExecutionPolicy Bypass -Scope Process -Force; & ([scriptblock]::Create((irm https://raw.githubusercontent.com/vlad907/video-wall-control/main/setup-receiver.ps1))) -MediaServer MEDIA-PC
# Safe to run again: it skips what's installed, pulls, rebuilds and restarts.
param(
    [string]$MediaServer = "",            # name or IP of the PC running the engine (required)
    [int]$EnginePort = 8090,
    [string]$ApiKey = "",                 # the engine's editor PIN or API key, if it has one
    [string]$InstallDir = "C:\video-wall-control",
    [string]$RepoUrl = "https://github.com/vlad907/video-wall-control.git",
    [string]$Branch = "main",
    [string]$NdiSdkDir = "C:\Program Files\NDI\NDI 6 SDK",
    [string]$NdiSdkInstaller = "",        # a downloaded "NDI 6 SDK.exe", if the automatic download fails
    [string]$NdiSdkUrl = "https://downloads.ndi.tv/SDK/NDI_SDK/NDI%206%20SDK.exe",
    [string]$NdiToolsUrl = "https://downloads.ndi.tv/Tools/NDI%206%20Tools.exe",
    [switch]$SkipNdiTools
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is very slow with the progress bar

# --- Administrator (re-launches itself elevated, keeping the arguments) ------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $self = $PSCommandPath
    if (-not $self) {
        # Run from a pasted command (irm): save a copy to re-launch elevated.
        $self = Join-Path $env:TEMP "setup-receiver.ps1"
        Invoke-WebRequest -UseBasicParsing -OutFile $self `
            -Uri "https://raw.githubusercontent.com/vlad907/video-wall-control/$Branch/setup-receiver.ps1"
    }
    $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", "`"$self`"")
    foreach ($kv in $PSBoundParameters.GetEnumerator()) {
        if ($kv.Value -is [switch]) { if ($kv.Value) { $argList += "-$($kv.Key)" } }
        else { $argList += @("-$($kv.Key)", "`"$($kv.Value)`"") }
    }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    return
}

$LogFile = Join-Path $env:TEMP "setup-receiver.log"
Start-Transcript -Path $LogFile -Append | Out-Null

function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host ""; Write-Host "FAILED: $msg" -ForegroundColor Red; Write-Host "Full log: $LogFile"; Stop-Transcript | Out-Null; throw $msg }

function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                [Environment]::GetEnvironmentVariable("Path", "User")
}

function Install-WingetPackage([string]$Id, [string[]]$Extra = @()) {
    $listed = winget list -e --id $Id --accept-source-agreements 2>$null | Out-String
    if ($listed -match [regex]::Escape($Id)) { Write-Host "${Id}: already installed."; return }
    Write-Host "${Id}: installing..."
    winget install -e --id $Id --silent --accept-source-agreements --accept-package-agreements @Extra
    # -1978335189: already installed / no newer version. Anything else non-zero is a failure.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) { Fail "winget couldn't install $Id (exit code $LASTEXITCODE)." }
    Refresh-Path
}

if (-not $MediaServer) { Fail "Give the media server running the engine: -MediaServer NAME-OR-IP" }
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget not found. Install 'App Installer' from the Microsoft Store, then run this again."
}

# --- 1. Prerequisites ---------------------------------------------------------
Step "1/6  Installing prerequisites"
Install-WingetPackage "Git.Git"
Install-WingetPackage "Python.Python.3.12" @("--scope", "machine")
Install-WingetPackage "Kitware.CMake"

# Visual Studio 2022 Build Tools with the C++ workload. Checked with vswhere, since Build Tools
# can be installed without the C++ compiler.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
function Has-VcTools {
    if (-not (Test-Path $vswhere)) { return $false }
    $p = & $vswhere -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    return [bool]$p
}
if (Has-VcTools) {
    Write-Host "Visual Studio 2022 C++ tools: already installed."
} else {
    Write-Host "Visual Studio 2022 Build Tools (C++): installing, this downloads a few GB and takes a while..."
    $vsArgs = "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    $existing = if (Test-Path $vswhere) { & $vswhere -products Microsoft.VisualStudio.Product.BuildTools -version "[17.0,18.0)" -property installationPath } else { $null }
    if ($existing) {
        # Build Tools are there without C++: add the workload to that install.
        $setup = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\setup.exe"
        $p = Start-Process $setup -ArgumentList "modify --installPath `"$existing`" $vsArgs" -Wait -PassThru
        if ($p.ExitCode -notin 0, 3010) { Fail "Visual Studio Installer exited with $($p.ExitCode)." }
    } else {
        winget install -e --id Microsoft.VisualStudio.2022.BuildTools --accept-source-agreements --accept-package-agreements --override $vsArgs
    }
    if (-not (Has-VcTools)) { Fail "The Visual Studio C++ tools still aren't there. A reboot may be needed: restart and run this again." }
}

# NDI 6 SDK: headers and import library to build against, and the DLL the receiver runs with.
if (Test-Path (Join-Path $NdiSdkDir "Include\Processing.NDI.Lib.h")) {
    Write-Host "NDI 6 SDK: already installed."
} else {
    if (-not $NdiSdkInstaller) {
        $NdiSdkInstaller = Join-Path $env:TEMP "NDI 6 SDK.exe"
        Write-Host "NDI 6 SDK: downloading..."
        try {
            Invoke-WebRequest -Uri $NdiSdkUrl -OutFile $NdiSdkInstaller -UseBasicParsing
        } catch {
            Fail ("Couldn't download the NDI SDK ($($_.Exception.Message)). Download it from " +
                  "https://ndi.video/for-developers/ndi-sdk/ and run again with -NdiSdkInstaller `"C:\path\NDI 6 SDK.exe`".")
        }
    }
    Write-Host "NDI 6 SDK: installing (you accept the NDI SDK license by installing it)..."
    $p = Start-Process $NdiSdkInstaller -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-" -Wait -PassThru
    if ($p.ExitCode -ne 0 -or -not (Test-Path (Join-Path $NdiSdkDir "Include\Processing.NDI.Lib.h"))) {
        Fail "The NDI SDK installer didn't finish (exit code $($p.ExitCode)), or didn't install to '$NdiSdkDir' (use -NdiSdkDir)."
    }
}
# NDI Tools: Studio Monitor, which the dashboard plays with until the receiver takes over.
$StudioMonitor = Get-ChildItem "C:\Program Files\NDI\*\Studio Monitor\Application.Network.VideoMonitor2.x64.exe" -ErrorAction SilentlyContinue
if ($SkipNdiTools) {
    Write-Host "NDI Tools: skipped."
} elseif ($StudioMonitor) {
    Write-Host "NDI Tools: already installed."
} else {
    $toolsExe = Join-Path $env:TEMP "NDI 6 Tools.exe"
    Write-Host "NDI Tools: downloading (about 550 MB)..."
    try {
        Invoke-WebRequest -Uri $NdiToolsUrl -OutFile $toolsExe -UseBasicParsing
        Write-Host "NDI Tools: installing..."
        $p = Start-Process $toolsExe -ArgumentList "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Warning "NDI Tools installer exited with $($p.ExitCode); carrying on (only Studio Monitor fallback needs it)." }
    } catch {
        Write-Warning "Couldn't get NDI Tools ($($_.Exception.Message)); carrying on (only Studio Monitor fallback needs it)."
    }
}
Refresh-Path
foreach ($tool in "git", "cmake") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "$tool isn't on PATH after installing it. Open a new PowerShell and run this again." }
}

# --- 2. Code ------------------------------------------------------------------
Step "2/6  Getting video-wall-control into $InstallDir ($Branch)"
# Stop what runs from the folder, so its files can be replaced.
Stop-ScheduledTask -TaskName "VideoWallControl" -ErrorAction SilentlyContinue
Get-Process mosaic_receiver -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path (Join-Path $InstallDir ".git")) {
    git -C $InstallDir fetch origin $Branch
    if ($LASTEXITCODE -ne 0) { Fail "git fetch failed." }
    git -C $InstallDir checkout $Branch
    git -C $InstallDir pull --ff-only origin $Branch
    if ($LASTEXITCODE -ne 0) { Fail "git pull failed: local changes in $InstallDir? (git -C $InstallDir status)" }
} else {
    if (Test-Path $InstallDir) { Fail "$InstallDir exists but isn't a git checkout. Move it away or use -InstallDir." }
    git clone --branch $Branch $RepoUrl $InstallDir
    if ($LASTEXITCODE -ne 0) { Fail "git clone failed." }
}
$EngineDir = Join-Path $InstallDir "mosaic-engine"
$ReceiverExe = Join-Path $EngineDir "build\Release\mosaic_receiver.exe"

# --- 3. Build -----------------------------------------------------------------
Step "3/6  Building mosaic_receiver (Release)"
Push-Location $EngineDir
try {
    cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DNDI_SDK_DIR=$($NdiSdkDir -replace '\\','/')"
    if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed." }
    cmake --build build --config Release --target mosaic_receiver --parallel
    if ($LASTEXITCODE -ne 0) { Fail "Build failed." }
} finally { Pop-Location }
if (-not (Test-Path (Join-Path (Split-Path $ReceiverExe) "Processing.NDI.Lib.x64.dll"))) {
    Write-Warning "Processing.NDI.Lib.x64.dll isn't next to the receiver; it won't receive NDI."
}

Write-Host "Self-test (synthetic streams, about 5 s):"
& $ReceiverExe --self-test 4
if ($LASTEXITCODE -ne 0) { Fail "Receiver self-test failed (output above)." }

# --- 4. Dashboard config: where the engine runs --------------------------------
Step "4/6  Pointing the dashboard at the engine on ${MediaServer}:$EnginePort"
$ConfigPath = Join-Path $InstallDir "config.json"
$cfg = Get-Content $ConfigPath -Raw | ConvertFrom-Json
$cfg | Add-Member -Force -NotePropertyName mosaic -NotePropertyValue ([pscustomobject]@{
    enabled = $true; exe_path = ""; port = $EnginePort; args = @(); host = $MediaServer })
$cfg | ConvertTo-Json -Depth 20 | Set-Content -Path $ConfigPath -Encoding UTF8
try {
    $null = Invoke-RestMethod -Uri "http://${MediaServer}:$EnginePort/api/receiver/package" -TimeoutSec 5
    Write-Host "The engine answers."
} catch {
    Write-Warning "Can't reach the engine at http://${MediaServer}:$EnginePort ($($_.Exception.Message))."
    Write-Warning "The dashboard keeps trying. Check the engine runs there and its firewall allows TCP $EnginePort."
}

# --- 5. Dashboard: scheduled task, firewall, start -----------------------------
Step "5/6  Installing and starting the dashboard"
& (Join-Path $InstallDir "install.ps1")   # adds the receiver's NDI firewall rule too, now the exe exists

# --- 6. Register this PC with the engine, player = Mosaic Receiver -------------
Step "6/6  Setting this PC's player to Mosaic Receiver on the engine"
$api = "http://${MediaServer}:$EnginePort/api/endpoints"
$headers = @{}
if ($ApiKey) { $headers["X-Api-Key"] = $ApiKey }
try {
    $list = Invoke-RestMethod -Uri $api -Headers $headers -TimeoutSec 10
    $me = $env:COMPUTERNAME.ToLower()
    $mine = @($list.endpoints) | Where-Object { $_.host -and ($_.host.ToLower().Split(".")[0] -eq $me) } | Select-Object -First 1
    if ($mine) {
        $null = Invoke-RestMethod -Method Put -Uri "$api/$($mine.id)" -Headers $headers -ContentType "application/json" `
            -Body (@{ player = "mosaic" } | ConvertTo-Json) -TimeoutSec 10
        Write-Host "Endpoint '$($mine.name)' now plays with the Mosaic Receiver."
    } else {
        $null = Invoke-RestMethod -Method Post -Uri $api -Headers $headers -ContentType "application/json" `
            -Body (@{ name = $env:COMPUTERNAME; host = $env:COMPUTERNAME; player = "mosaic" } | ConvertTo-Json) -TimeoutSec 10
        Write-Host "Added endpoint '$env:COMPUTERNAME' with the Mosaic Receiver as its player."
    }
    Write-Host "Which output goes on which screen is set in the editor: http://${MediaServer}:$EnginePort -> Endpoints."
} catch {
    Write-Warning "Couldn't set the player on the engine ($($_.Exception.Message))."
    Write-Warning "If the engine has an editor PIN, re-run with -ApiKey PIN; or set it in the editor: Endpoints -> $env:COMPUTERNAME -> Player: Mosaic Receiver."
}

Write-Host ""
Write-Host "Waiting for the receiver (up to 2 minutes)..."
$deadline = (Get-Date).AddMinutes(2)
$running = $false
while ((Get-Date) -lt $deadline) {
    try { $null = Invoke-RestMethod -Uri "http://127.0.0.1:8095/health" -TimeoutSec 2; $running = $true; break }
    catch { Start-Sleep -Seconds 3 }
}
Write-Host ""
if ($running) {
    Write-Host "DONE. Mosaic Receiver is running. Status: http://127.0.0.1:8095/status" -ForegroundColor Green
} else {
    Write-Host "DONE, except the receiver hasn't started yet." -ForegroundColor Yellow
    Write-Host "The dashboard (http://localhost:8080) starts it once this PC's player is Mosaic Receiver in the editor."
}
Write-Host "Logs: $InstallDir\logs (mosaic-receiver.log, videowall.log). Setup log: $LogFile"
Stop-Transcript | Out-Null
