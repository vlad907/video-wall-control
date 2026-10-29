# Windows / Direct3D 11

## Status

`engine/src/render/d3d11/d3d11_renderer.cpp` has been:
- **cross-compiled and linked** into a Windows x64 executable with MinGW-w64 (GCC 16), with no warnings
- **HLSL-validated**: all three entry points (`vs_main`, `ps_pattern`, `ps_textured`) compile with glslang's HLSL front end

It has **not been run on Windows hardware**. Below is what to check on its first run.

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
- NDI input frames are uploaded into `DYNAMIC` textures with `Map(WRITE_DISCARD)`.
  Three textures rotate per source.
