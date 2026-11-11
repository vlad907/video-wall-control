# Keeps the mosaic engine running: starts it, restarts it if it exits or stops answering.
# Started hidden at logon by the "MosaicEngine" scheduled task (install-engine.bat).
param(
    [int]$Port = 8090,
    [string]$Layout = ""
)

$EngineDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $EngineDir "build\Release\mosaic_engine.exe"
$LogDir = Join-Path (Split-Path -Parent $EngineDir) "logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Log = Join-Path $LogDir "mosaic-engine.log"
$Watchdog = Join-Path $LogDir "mosaic-engine-watchdog.log"

function Note($msg) {
    $line = "{0} {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $msg
    Add-Content -Path $Watchdog -Value $line
}

function Rotate($path) {
    if ((Test-Path $path) -and (Get-Item $path).Length -gt 5MB) { Move-Item -Force $path "$path.1" }
}

$engineArgs = @("--port", $Port)
if ($Layout) { $engineArgs += @("--layout", $Layout) }

# At boot this runs before the network is up. NDI picks its network adapters once, when the engine
# starts: started too early, it never finds any stream. So wait (up to 2 minutes) until the address
# NDI is limited to (ndi-config.v1.json "adapters.allowed") is up, or any real IPv4 address if none.
function NdiAddresses {
    foreach ($f in "$env:ProgramData\NDI\ndi-config.v1.json", "$env:APPDATA\NDI\ndi-config.v1.json") {
        try {
            $cfg = Get-Content $f -Raw -ErrorAction Stop | ConvertFrom-Json
            if ($cfg.ndi.adapters.allowed) { return @($cfg.ndi.adapters.allowed) }
        } catch {}
    }
    return @()
}
function NetworkReady {
    $up = @(Get-NetIPAddress -AddressFamily IPv4 -AddressState Preferred -ErrorAction SilentlyContinue |
            Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } | ForEach-Object { $_.IPAddress })
    $want = NdiAddresses
    if ($want.Count) { return [bool]($want | Where-Object { $up -contains $_ }) }
    return $up.Count -gt 0
}
$deadline = (Get-Date).AddMinutes(2)
if (-not (NetworkReady)) {
    Note "waiting for the network (NDI adapters: $((NdiAddresses) -join ', '))"
    while (-not (NetworkReady) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
    Start-Sleep -Seconds 5  # the address is there; give the link and its multicast a moment
    Note $(if (NetworkReady) { "network up" } else { "network still not up after 2 minutes; starting anyway" })
}

while ($true) {
    if (-not (Test-Path $Exe)) {
        Note "engine not built yet ($Exe); checking again in 30 s"
        Start-Sleep -Seconds 30
        continue
    }
    Rotate $Log
    Rotate $Watchdog
    # One engine at a time: a copy left from before (or started by hand) is replaced.
    Get-Process mosaic_engine -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Note "starting $Exe $($engineArgs -join ' ')"
    $proc = Start-Process -FilePath $Exe -ArgumentList $engineArgs -WorkingDirectory $EngineDir -WindowStyle Hidden `
        -RedirectStandardError $Log -RedirectStandardOutput "$Log.out" -PassThru
    $null = $proc.Handle  # keeps ExitCode readable after the process exits
    $misses = 0
    Start-Sleep -Seconds 5
    while (-not $proc.HasExited) {
        try {
            Invoke-WebRequest -UseBasicParsing -TimeoutSec 3 "http://127.0.0.1:$Port/api/health" | Out-Null
            $misses = 0
        } catch {
            $misses++
            if ($misses -ge 6) {
                Note "engine not answering for 30 s; restarting it"
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                break
            }
        }
        Start-Sleep -Seconds 5
    }
    if ($proc.HasExited) { Note "engine exited with code $($proc.ExitCode); restarting in 3 s (see logs\mosaic-engine.log)" }
    Start-Sleep -Seconds 3
}
