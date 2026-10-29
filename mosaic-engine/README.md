# NDI Mosaic Engine

A GPU mosaic mapper: it receives NDI video (or renders a test pattern), places it on
a virtual canvas, cuts arbitrary regions out of that canvas with independent crop,
scale and rotation, and publishes each region as its own NDI stream. A web editor
in the style of Resolume's Advanced Output drives the running engine through a REST API.

```
 NDI inputs ─┐                       ┌─► output RT ─► readback ring ─► sender thread ─► NDI "Mosaic_Output_01"
 test pattern ┼─► canvas texture ─────┼─► output RT ─► readback ring ─► sender thread ─► NDI "Mosaic_Output_02"
 solid colour ┘   (one GPU pass)      └─► ... one per output, all from the same frame id

 web editor (React/TS) ◄──REST + SSE──► HTTP server ──► LayoutStore (JSON files) ──► render thread
```

## Status

| Piece | State |
|---|---|
| Render engine, test pattern, crop / scale / rotation, bounded readback, metrics | **Working, measured** on macOS (Metal, Apple M4) |
| GPU UYVY 4:2:2 output (BT.709), default wire format | **Working, verified** (`tools/check_uyvy.py`); halves readback |
| Precise frame pacing + real-time thread priority | **Working** on macOS (tick lateness about 0.001 ms); Windows path (high-resolution timer, MMCSS) compiled, not run |
| Web editor (drag / resize regions, inspector, live previews, layouts, metrics) | **Working**, verified against the running engine |
| REST API + server-sent events | **Working**, exercised with curl |
| Layout persistence across restarts | **Working**, verified |
| Direct3D 11 backend (Windows) | **Compiles and links** for Windows x64 (MinGW cross-build); HLSL validated with glslang. **Not yet run on Windows hardware.** |
| NDI send / receive / discovery | **Written, not compiled.** The NDI SDK was not installed on the development machine. Builds automatically when CMake finds the SDK. See [docs/NDI.md](docs/NDI.md). |

Without the NDI SDK, outputs go to a *null sink* that reads the readback memory
and measures everything except the network send.

## Quick start

### Windows (target platform)

Requirements: Windows 10 1703+ or 11, Visual Studio 2022 (Desktop C++ workload),
CMake 3.24+, Node.js 18+, Git, and optionally the NDI 6 SDK.

```powershell
git clone https://github.com/vlad907/video-wall-control.git
cd video-wall-control\mosaic-engine
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNDI_SDK_DIR="C:/Program Files/NDI/NDI 6 SDK"
cmake --build build --config Release
cd ui && npm ci && npm run build && cd ..
build\Release\mosaic_engine.exe
```

Open http://localhost:8090. See [docs/WINDOWS.md](docs/WINDOWS.md) for what to check on first run.

### macOS (development)

```bash
brew install cmake ninja node
cmake -S . -B build -G Ninja
cmake --build build
(cd ui && npm ci && npm run build)
./build/mosaic_engine
```

### Command line

```
mosaic_engine [--port 8090] [--host 0.0.0.0] [--data ./data] [--layouts ./layouts]
              [--ui ./ui/dist] [--layout ID] [--run-seconds N] [--snapshot-dir DIR]
```

- `--data`: where layouts and the active-layout state are saved. It's seeded from `--layouts` on first run.
- `--run-seconds N`: headless benchmark; prints the metrics JSON when it exits.
- `--snapshot-dir DIR`: after 2 s, writes a full-resolution JPEG of every output plus `canvas.jpg`. This is the quickest way to check a mapping.

Tools:
- `python3 tools/bench.py`: runs each layout in BGRA and UYVY and prints a results table.
- `python3 tools/check_uyvy.py`: checks GPU UYVY packing against BGRA. Needs numpy for the full report.

A test-pattern source with `"animated": false` freezes the moving bar and frame counter, so snapshots can be compared.

The API has no authentication. Use `--host 127.0.0.1` if the machine is on an untrusted network.

## Layouts

Example layouts are in [layouts/](layouts):

| File | What it shows |
|---|---|
| `quad-1080p.json` | Phase 1 test: 3840×2160 canvas cut into 4 × 1080p outputs |
| `wall-2x2-4k.json` | 2×2 wall of 4K displays: 7680×4320 canvas, regions (0,0) (3840,0) (0,2160) (3840,2160), each 3840×2160 |
| `row-1x4-4k.json` | 1×4 horizontal: 15360×2160 canvas |
| `mixed-resolution.json` | 4K centre plus 1080p sides, using `fit`, `fill` and a 1:1 pixel crop |
| `rotated-portrait.json` | Portrait displays (`rotation` 90 / 270) beside a landscape pair |

Output fields: `id`, `name`, `ndiName`, `width`/`height` (output resolution), `pixelFormat` (`uyvy`, the default, or `bgra`), `crop`
(region in canvas pixels, or in source pixels when `source` is a source id),
`rotation` (0/90/180/270, clockwise), `scaleMode` (`stretch` | `fit` | `fill`),
`source` (`"canvas"` or a source id), `enabled`.

Rotation is clockwise. A display physically mounted 90° counter-clockwise needs `rotation: 90`.

## The test pattern

Every 1920×1080 tile of the pattern has:
- a big tile number
- the tile's source origin in pixels (x on the left, y on the right)
- corner markers: top-left **red**, top-right **green**, bottom-left **blue**, bottom-right **white**
- an up-arrow
- a 60/240 px grid (count squares to check scaling)
- a coloured border

Across the whole source there's also:
- a circle, which is only round if the aspect ratio is preserved
- a bar that crosses the source every 4 s (motion and tearing)
- the engine frame id, both as digits and as a 24-bit binary strip. Every output rendered from one frame shows the same number.

## REST API

| Method | Path | |
|---|---|---|
| GET | `/api/sources` | configured sources + status, discovered NDI sources |
| POST / PATCH / DELETE | `/api/sources[/:id]` | edit sources of the active layout |
| GET | `/api/outputs` | outputs of the active layout |
| POST | `/api/outputs` | add an output (partial body; defaults filled in) |
| PATCH | `/api/outputs/:id` | change any field; applied on the next frame |
| DELETE | `/api/outputs/:id` | remove |
| GET | `/api/outputs/:id/snapshot.jpg` | full-res frame (header `X-Frame-Id`) |
| GET | `/api/outputs/:id/frame.bgra` | the same frame as raw BGRA (UYVY decoded); headers `X-Width`, `X-Height`, `X-Frame-Id` |
| GET | `/api/preview/:target.jpg` | low-res live preview, `target` = `canvas` or an output id |
| GET / PUT | `/api/layout` | the whole active layout |
| GET | `/api/layouts` | saved layouts |
| GET / DELETE | `/api/layouts/:id` | |
| POST | `/api/layouts` | save a layout (full body), or `{"fromActive":true,"name":"..."}` to save a copy |
| POST | `/api/layouts/:id/activate` | switch live |
| GET | `/api/metrics` | engine, per-output and sync metrics |
| GET | `/api/events` | server-sent events: metrics + layout version every 500 ms |

Examples:

```bash
curl -X PATCH localhost:8090/api/outputs/out-2 -d '{"crop":{"x":1380,"y":320},"rotation":270}'
curl -X POST  localhost:8090/api/layouts/wall-2x2-4k/activate -d ''
```

Use `-d ''` on POSTs without a body: the HTTP library rejects a POST without a `Content-Length` header.

Status and metrics are pushed with **server-sent events**, not WebSockets. Everything
the UI needs is server-to-client, and SSE needs no extra dependency, works through
proxies, and reconnects automatically. Commands go over REST.

## Measured performance (macOS, Apple M4, Metal, null sink)

From `python3 tools/bench.py --seconds 15`:

| Layout | Format | fps | Late frames | Tick lateness | CPU / frame | GPU / frame | GPU util | Readback | Frame → sink | Drops | Frames on all outputs |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 4 × 1080p | BGRA | 60.02 | 0 | 0.001 ms | 0.41 ms | 11.1 ms | 71 % | 1989 MB/s | 12.5 ms | 0 | 668 / 668 |
| 4 × 1080p | **UYVY** | 60.01 | 0 | 0.002 ms | 0.42 ms | 11.9 ms | 76 % | **996 MB/s** | 13.3 ms | 0 | 665 / 665 |
| 4 × 4K | BGRA | 60.00 | 0 | 0.001 ms | 0.33 ms | 14.5 ms | 89 % | 7957 MB/s | 15.6 ms | 0 | 666 / 666 |
| 4 × 4K | **UYVY** | 59.99 | 0 | 0.001 ms | 0.37 ms | 14.8 ms | 89 % | **3981 MB/s** | 15.9 ms | 0 | 666 / 666 |

Memory stays flat at about 267 MB over a 40 s 4×4K run.

How to read these numbers:
- **UYVY halves readback bandwidth.** The packing accuracy is checked by `tools/check_uyvy.py`: on pixel pairs of uniform colour (96.7 % of the test frame), the maximum error is 1 level.
- **Tick lateness** is how late each frame starts compared with its schedule. Before the render thread used the macOS real-time policy, it averaged 3–4 ms (timer coalescing). See `engine/src/util/realtime.cpp`.
- **GPU / frame and utilisation** are mostly the test pattern. Every frame it redraws all 33 MP of the 8K canvas with per-pixel digit drawing. A real NDI input replaces that with one texture upload. macOS also scales the GPU clock down, so these times are not full-speed costs.
- **Frame → sink** is from the frame tick to the sink accepting the frame. It is mostly GPU execution time at the reduced clock. It does not include NDI encoding, the network, or the receiver.
- These numbers come from Apple silicon's unified memory, where a readback costs no PCIe transfer. On a discrete GPU, readback is the cost UYVY halves, so that's the first thing to measure on Windows.

## Synchronisation: what is and isn't guaranteed

See [docs/SYNC.md](docs/SYNC.md). In short:
- All outputs of a frame are rendered from the same canvas in one GPU submission and carry the same frame id and NDI timecode. That is verified by the `frames on all outputs` metric.
- NDI transport timing and when each receiver displays a frame are **not** controlled by this engine.

## Limitations / next steps

- The Direct3D 11 backend and NDI integration need their first run on the Windows workstation (run `tools/bench.py` there). See [docs/NEXT.md](docs/NEXT.md).
- No audio.
- No video-file source.
- No edge blending or warping.
- One canvas per layout.
