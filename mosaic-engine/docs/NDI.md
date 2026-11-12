# NDI integration

## Status

`engine/src/io/ndi_sdk.cpp` implements sending, receiving and discovery against
the NDI SDK C API (v5/v6). It is built with the NDI 6 SDK on Windows and has been used
with an NDI input and 4 NDI outputs playing in NDI Studio Monitor (2-hour run). CMake
compiles it when it finds the SDK. Otherwise `ndi_none.cpp` is used, and every output goes
to a measuring null sink (that is how the macOS and CI builds run).

The engine only depends on the small interface in `engine/src/io/video_io.hpp`
(`VideoSink`, `NdiReceiver`, `NdiSystem`), so if the SDK's API differs from what's
expected, the fix stays inside `ndi_sdk.cpp`.

## Install the SDK

1. Download the **NDI SDK** from https://ndi.video/for-developers/ndi-sdk/ (free, registration required).
2. Install it:
   - **Windows**: run the *NDI 6 SDK* installer. The default location is `C:\Program Files\NDI\NDI 6 SDK`, containing `Include\`, `Lib\x64\` and `Bin\x64\`.
   - **macOS**: run the *NDI SDK for Apple* package. It installs to `/Library/NDI SDK for Apple`, containing `include/` and `lib/macOS/libndi.dylib`.
3. Configure:
   ```powershell
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNDI_SDK_DIR="C:/Program Files/NDI/NDI 6 SDK"
   cmake --build build --config Release
   ```
   CMake should print `NDI SDK found: ...`. On Windows the build copies
   `Processing.NDI.Lib.x64.dll` next to `mosaic_engine.exe`.
4. Run the engine. The log should show `[ndi] NDI SDK ...`, and the metrics' `Sink` column should read `ndi`.

## Verify

1. Start the engine with the `quad-1080p` layout.
2. On the same machine or another one on the LAN, open **NDI Studio Monitor** (NDI Tools).
   Four sources should appear: `<HOST> (Mosaic_Output_01)` through `(Mosaic_Output_04)`.
3. Open each one. Each should show a different tile number (1 to 4), with the corner
   colours in the right places, and the same frame counter on all four.
4. The `Rx` column in the editor's metrics table shows how many receivers are connected to each stream.
5. Add an NDI input: Sources → *Add NDI source* lists everything NDI discovery finds.
   Click ⤢ to stretch it over the canvas.

## How frames are submitted

- **Send:** `NDIlib_send_send_video_async_v2` with `NDIlib_FourCC_video_type_UYVY` (the default, packed on the GPU)
  or `..._BGRA` per output. `p_data` points straight at the GPU readback buffer: a shared `MTLBuffer` on
  macOS, or a mapped `STAGING` texture on Windows with `line_stride_in_bytes = RowPitch`.
  No CPU copy happens before NDI.
- **Buffer lifetime:** with async sends, NDI may read the buffer until the *next*
  `send_video_async` call returns. The output channel therefore holds one slot
  back and releases it after the following send, and `flush()` (a null frame)
  is called on shutdown. Each output has 4 slots: one held by NDI, up to 2
  queued, one being written by the GPU. If the sender falls behind, the oldest
  queued frame is dropped, so latency can't build up.
- **GPU textures:** the NDI SDK only accepts CPU memory for uncompressed video.
  There's no D3D or Metal texture submission. So one GPU→CPU readback per output
  per frame is unavoidable, and the design keeps it to exactly one.
- **Timing:** `clock_video = false`, so the engine paces frames itself. Every
  output of a frame gets the same `timecode`, plus metadata
  `<mosaic frame_id="N"/>` so tools can check frame alignment.
- **Receive:** one connection and one thread per stream, shared by every canvas and source that
  shows it. It calls `NDIlib_recv_capture_v3` with `NDIlib_recv_color_format_UYVY_BGRA`: streams
  without transparency arrive as UYVY (NDI's native format, no conversion on the CPU) and are
  unpacked to RGB on the GPU (`ps_unpack_uyvy`, BT.709); streams with transparency arrive as BGRA.
  Only the newest frame is kept; each canvas's render thread uploads it into one of three rotating
  textures. When frames stop, the source shows "no signal (holding last frame)". The source's
  status has `inputFps`, `sentFps` (the sender's stated rate) and `framesDropped`.

## Network setup on the media server

NDI's settings are in `C:\ProgramData\NDI\ndi-config.v1.json` (and `%APPDATA%\NDI\`); NDI Access
Manager edits the same file. The engine reads them when it starts, so restart it after a change.

- **A PC with two network adapters.** Limit NDI to the one on the video network, or it may announce
  and send on the other: `"adapters": {"allowed": ["<that adapter's IP>"]}`. Set that adapter's
  network profile to **Private** in Windows; on a Public network NDI's discovery is blocked.
- **Streams discovery doesn't find** (encoders on another subnet, or mDNS filtered by a switch): list
  their addresses under `"networks": {"ips": "192.168.1.31,192.168.1.201"}` (Access Manager →
  *Remote Sources*). A receiving PC that can't see the media server's streams gets the media
  server's IP there in the same way.
- **Fixed addresses.** Those lists hold IP addresses, so give the media server and the encoders
  DHCP reservations.
- **Boot.** `run-engine.ps1` waits up to 2 minutes for the allowed adapter's address (or any real
  IPv4 address) before starting the engine, and notes it in `logs\mosaic-engine-watchdog.log`.
  Started before the network is up, NDI finds no streams until the engine is restarted.
- **Firewall.** `install-engine.bat` adds rules for the engine's NDI streams and port 8090.

## Bandwidth and UYVY

A 3840×2160 BGRA frame is 33 MB. At 60 fps that's about 2 GB/s per output, and about 8 GB/s for four 4K outputs.
The output pass maps, samples and packs BT.709 limited-range UYVY 4:2:2 in one shader (`ps_textured_uyvy` in
`render/shaders.hpp`). Measured at 4×4K, that halves the readback to 3.98 GB/s, and NDI
receives its native format, so it does no colour conversion of its own. Set an output's
`pixelFormat` to `bgra` to compare.
