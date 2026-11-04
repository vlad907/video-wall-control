# Mosaic Receiver: plan

A receiving-PC player to replace NDI Studio Monitor on endpoints. It plays the engine's
NDI outputs full screen and shows them **in step**: on one PC first, then across PCs.

Status: **Phase 1 built; not yet run on the Windows hardware.** Studio Monitor stays supported,
and each endpoint chooses its player, so a PC can switch back at any time.

## Using it (phase 1)

1. **Nothing to install by hand.** The media server builds `mosaic_receiver.exe` with the engine and hands
   it out (`GET /api/receiver/package`, `/api/receiver/files/...`), together with the NDI DLL.
   - **Each receiving PC's dashboard** compares that version with its own on every check-in. When they differ, it
     downloads the files, swaps them in, and restarts the receiver, which takes a few seconds. After `update.bat`
     on the media server, every receiving PC updates itself within about 5 seconds.
   - **The endpoint panel** shows "Receiver up to date (version)", or what went wrong.
   - **Firewall:** run `install.bat` once on each receiving PC after its first download, to add the receiver's
     firewall rule.
2. **In the editor:** *Endpoints* → the PC → **Player: Mosaic Receiver**. A PC only switches once it has the
   receiver. If the download fails, it stays on Studio Monitor (the screens never go black) and the endpoint
   panel says why.
3. **On the PC, within a few seconds,** the dashboard:
   - stops the Studio Monitor windows
   - starts the receiver, with one full-screen window per display on its screen
   - restarts the receiver if it stops
   - brings it back after a reboot (the setup is saved in `config.json`)

   The PC's dashboard shows *Endpoint: … (Mosaic Receiver)*. The receiver's log is `logs\mosaic-receiver.log`.
4. **The endpoint panel shows:**
   - how often the PC's displays were in step, and the most frames they were apart in the last 10 s
   - **how many refreshes per second it really presents** (if this is well under the monitors' Hz, the PC can't keep up)
   - **the upload time per refresh**
   - per display: the frame it shows, the new pictures per second it shows, and how many refreshes it was late

   A stream up to **6 frames** behind the others is waited for. That's 100 ms at 60 fps, enough to ride out uneven arrival.

   **Measured on the glass.** After every present, the receiver asks Windows which present each monitor actually
   displayed and when (DXGI frame statistics). That gives:
   - **"On screen, displays apart"**: average and maximum, in milliseconds, between the first and the last
     display to show each engine frame (last 10 s). Under one refresh (16.7 ms at 60 Hz) is as close as separate
     monitors get without spanning or framelock.
   - **per display, "+N ms (max M)"**: how much later than the earliest display it showed each frame.
   - **"N repeated"**: refreshes where a monitor showed a picture again because the receiver didn't present in
     time. This is stutter caused by the receiving PC.

   **Flash test** (endpoint panel): every display flashes white on each engine frame divisible by 30, twice a
   second at 60 fps. Displays in step flash together. Film the wall in slow motion to compare against the numbers,
   including across receiving PCs, since the flash follows the engine's frame number.
   `/api/control` lists `syncSpreadFrames` per endpoint.
5. **Switch back** by choosing *NDI Studio Monitor*: the receiver stops and the Studio Monitors return.

Checks without hardware:
- `mosaic_receiver --self-test 6` runs four synthetic streams (on time, late with jitter, dropping frames,
  stalling) and checks that the in-step displays show the same frame. It runs in Windows CI on every push,
  together with a short smoke test of the Direct3D 11 presenter on WARP.
- `mosaic_receiver --config FILE` with `"stream": "synthetic"` displays shows moving bars on real windows,
  so frame alignment can be watched on the actual monitors.

What phase 1 doesn't do yet:
- ~~Frames are uploaded as BGRA~~: the receiver now takes **UYVY** from NDI (what the engine sends, half the bytes,
  no CPU colour conversion) and converts on the GPU.
- **A lost stream holds its last frame** on the receiver. The engine-side fallback slate covers inputs to the engine.
- **No editor screen for spanned displays (Eyefinity) yet.** The receiver already supports tiles inside one window.
- **Cross-PC alignment is phase 2.**

## Why

| | Studio Monitor today | Mosaic Receiver |
|---|---|---|
| Frame alignment between displays | none: each window shows frames as they arrive | frame N goes on every display of the PC in the same refresh; across PCs, at the same clock time |
| What's on screen | unknown (we only know which source it plays) | reports the frame id and time each display showed, so the engine can show the real offset between screens and the end-to-end latency |
| Window placement | set over its HTTP API, can drift onto the wrong screen | opens full screen on its assigned monitor by itself |
| Lost signal | frozen last frame | slate (the group's fallback image, or "no signal"); a local slate when the media server is gone |
| Buffering / latency | Studio Monitor's own | ours: a small fixed jitter buffer, tunable |
| Later | — | per-display colour / brightness matching, identify-display overlay, local test patterns |

Display-level limits stay: screens on different PCs refresh on their own clocks, so without
hardware framelock (Quadro Sync, FirePro S400) cross-PC alignment is "within about one refresh",
not scan-line exact. See [SYNC.md](SYNC.md).

## Shape

```
engine ──NDI──► mosaic_receiver.exe (one per receiving PC, all its displays)
                  ├─ one NDI receive thread per stream ── frame queue (keeps frame id, timecode, NDI timestamp)
                  ├─ render thread: picks the frame to show, draws every display, presents together
                  └─ local HTTP (127.0.0.1:8095): config in, status out
dashboard (videowall.py) ── supervises it like Studio Monitor today, pushes the endpoint
                            assignment, reads its status and passes it on in the check-in
```

- **Same repo, same build.** A second executable in `mosaic-engine/` (CMake target `mosaic_receiver`),
  reusing `io/ndi_sdk.cpp`, the D3D11 setup and the shader code. Windows / D3D11 first; it builds on
  macOS (Metal) too, for development.
- **The dashboard stays the supervisor and the single check-in path.** An endpoint gets a `player`
  setting (`"studio_monitor"` default, or `"mosaic"`). With `"mosaic"` the dashboard starts
  `mosaic_receiver.exe` instead of Studio Monitor instances, gives it the displays (stream, screen,
  fallback image) and restarts it if it stops. Nothing else on the PC changes.

## How the alignment works

**Inputs.** Every engine output frame already carries:
- the engine frame id, in NDI metadata (`<mosaic frame_id="N"/>`)
- the same NDI `timecode` on every output of a frame
- the sender's NDI `timestamp`

The NDI receiver code becomes a small **queue** per stream (the last ~8 frames with those three values)
instead of today's "newest frame wins". It asks for **UYVY**, NDI's native format, so there's no CPU
colour conversion: it's uploaded as is and converted in the shader.

**One PC (phase 1).** Every display refresh, the render thread:
1. Picks the newest frame id that **every** stream on this PC has.
2. Draws that frame on all displays.
3. Presents all swap chains together.

A stream that falls behind for longer than a deadline (2 refreshes, say) doesn't hold the others back:
its display keeps the previous frame, and that counts as a *late* frame in the status.

Presentation:
- one borderless full-screen window per monitor
- flip-model swap chains (`FLIP_DISCARD`, waitable), so DWM doesn't add a frame
- one D3D11 device

Monitors on one GPU still scan out on independent clocks, so "same refresh" means within one refresh
interval (≤ 16.7 ms at 60 Hz). There's one exception: when the PC's displays are joined into a single desktop
surface (AMD Eyefinity / NVIDIA Mosaic), the receiver draws **one** window across all of them, and they
are then exactly in step. The receiver detects this and supports it.

**Across PCs (phase 2).**
- Each receiver keeps an estimate of the media server's clock: an NTP-style exchange with the engine over
  HTTP every few seconds, which is good to ~1 ms on a LAN.
- It shows a frame at `send time + fixed delay` (for example 50 ms, set per layout) on its own clock, at
  the refresh nearest that moment. Every PC aims at the same instant.
- Frames that would be shown late are dropped rather than shown out of step.
- The delay is the price: a little latency for alignment.

## Telemetry and measuring

Per display the receiver reports:
- frame id shown
- present time (from DXGI frame statistics)
- late / dropped counts
- decode fps

Per PC it reports the clock offset.

The engine combines these per wall group:
- **sync spread**: the largest difference, in frames, between displays at the same moment
- **end-to-end latency**: engine render to on screen

They appear in the endpoint view, the Monitor page and `/api/control`.

A **flash test** mode (every display flashes white on frame ids divisible by 60) makes the offset easy
to film with a phone, to check the numbers against reality.

## Phases

### Phase 1: the receiver on one PC
- `mosaic_receiver` app: NDI queues, UYVY upload, multi-monitor full-screen flip-model presentation,
  frame-aligned switching, fallback slate, local HTTP config / status, spanned-surface (Eyefinity) mode.
- Dashboard: `player` per endpoint, starts / supervises the receiver, passes its status in the check-in.
- Engine + editor: player choice per endpoint, per-display "frame shown / late" in the endpoint view,
  sync spread per PC.
- **Done when:** on the 610M Mini PC with 4 × 1080p60 displays, a 24-hour run shows
  - 60 fps on every display
  - sync spread 0 frames between its displays (telemetry, confirmed on film)
  - no more than one frame more latency than Studio Monitor
  - switching an endpoint between players works both ways

### Phase 2: across PCs
- Clock sync with the engine, scheduled presentation, layout-level presentation delay, and cross-PC sync
  spread per wall group.
- **Done when:** a wall spread over 2+ receiving PCs shows a sync spread of ≤ 1 frame in telemetry and
  on film, for an hour, while presets switch.

### Phase 3: extras
- Per-display colour matching (gain / offset / gamma per channel, set in the editor)
- An identify overlay (big display numbers)
- Local test patterns (grid, colour bars, the flash test)
- Optional on-screen status when the media server is unreachable

## Risks and what's needed before starting

- **Decoding load.** Full-NDI (SpeedHQ) decoding is CPU work in the NDI library; that's the same for Studio
  Monitor, so it isn't new. NDI|HX inputs are decoded on the media server, not here.
- **Monitor refresh rates.** Every display on a PC should run at the same rate (60 Hz). Mixed rates can't
  be aligned and get flagged.
- **Windows full-screen behaviour.** Flip-model borderless windows normally get independent flip, so there's
  no added frame. This has to be checked on the 610M and the AMD driver.
- **Information needed from the hardware test:**
  - which receiving PCs drive which displays, and how many each
  - each PC's GPU, and whether Eyefinity is available
  - whether the displays all run at 60 Hz
  - a slow-motion film of the frame-number strip across the current Studio Monitor setup (the baseline to beat)
