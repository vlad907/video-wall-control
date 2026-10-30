# Windows / Direct3D 11

## Status

The Direct3D 11 backend runs on Windows hardware: an AMD Radeon 610M Mini PC ran 4 NDI
outputs from an NDI input for 2 hours with the NDI 6 SDK. GitHub Actions also builds it with
MSVC on every push and checks its pixels on WARP (`tools/ci_check.py`, `tools/check_uyvy.py`).

**Integrated GPUs.** The first run on the 610M was GPU-bound at about 30 fps. Since then the engine
skips the canvas pass when one source fills the canvas, packs UYVY in the output pass, and skips
hidden sources (see [NEXT.md](NEXT.md)). If it's still under 60 fps:
- use an NDI input rather than the animated test pattern, or set the pattern's `animated` to false
- keep the canvas no larger than the wall needs

**Starting at boot.** Run the dashboard's `install.bat` (repo root) after building. The service
starts the engine at logon, before the Studio Monitors, restarts it if it stops answering, and
logs to `logs\mosaic-engine.log`. The dashboard's **Show mosaic on panels** button points each
Studio Monitor at the matching `Mosaic_Output_NN` stream.

Below is the checklist for a first run on a new machine.

## Build (Visual Studio 2022)

```powershell
winget install -e --id Kitware.CMake
winget install -e --id OpenJS.NodeJS.LTS
# Visual Studio 2022 with "Desktop development with C++"
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNDI_SDK_DIR="C:/Program Files/NDI/NDI 6 SDK"
cmake --build build --config Release
cd ui; npm ci; npm run build; cd ..
build\Release\mosaic_engine.exe --run-seconds 15 --snapshot-dir snap
```

## First-run checklist

The quickest full check is the benchmark script (needs Python 3):

```powershell
python tools\bench.py --seconds 30
python tools\check_uyvy.py build\Release\mosaic_engine.exe   # needs: pip install numpy
```

The benchmark prints fps, tick lateness, CPU and GPU time, readback MB/s, latency, drops
and complete frames for 4×1080p and 4×4K in both BGRA and UYVY. It saves full metrics to
`bench.json`. Please send me that file.

Step by step:


1. The log line `[gpu] Direct3D 11.4 on <your GPU>` appears.
   - If it says `Direct3D 11` instead, the fence path is unavailable and completions are polled. That works, but adds up to one frame of latency.
2. Open `snap\out-1.jpg` to `out-4.jpg`. They should show tiles 1 to 4, with red top-left / green top-right / blue bottom-left / white bottom-right corners.
3. The metrics JSON printed at exit should show:
   - `renderFps` ≈ 60
   - `lateFrames` ≈ 0
   - `framesPartial` = 0
   - every output with `droppedNoBuffer` / `droppedQueue` = 0
4. Repeat with `--layout wall-2x2-4k`. This is the real test on a discrete GPU, because each 4K output means 33 MB/frame of PCIe readback.
5. The `gpuUtilization` value comes from the "GPU Engine" performance counters. If it's -1, the counters are unavailable (rare, usually with some remote-desktop setups).

If shader compilation fails, the full `D3DCompile` error is printed. The HLSL is
generated from `engine/src/render/shaders.hpp`.

## Design notes

- Textures are `DXGI_FORMAT_B8G8R8A8_UNORM` throughout (the NDI BGRA layout).
- Each output renders into a DEFAULT render target, then `CopyResource`s into one
  of 4 STAGING textures. Once the frame's fence completes, the staging texture is
  mapped, and the mapped pointer and `RowPitch` go straight to the NDI sender. It
  stays mapped until the sender releases it, and is then unmapped on the render
  thread.
- `ID3D11Multithread::SetMultithreadProtected(TRUE)` is enabled because the
  completion thread maps staging textures on the immediate context.
- At most 2 frames are in flight, which bounds latency.
- Frame pacing: Windows' default sleep granularity is about 15.6 ms, so the render thread uses a
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` timer (Windows 10 1803+; otherwise `timeBeginPeriod(1)`)
  and spins for the last 0.7 ms. It also joins the MMCSS "Games" class. Check that `tickJitterMs`
  in the metrics is well under 1 ms.
- UYVY outputs are rendered by `ps_textured_uyvy` straight into a half-width target (map, sample
  and pack in one pass), so the staging readback is half the size and there is no BGRA intermediate.
- NDI input frames are uploaded into `DYNAMIC` textures with `Map(WRITE_DISCARD)`.
  Three textures rotate per source.
