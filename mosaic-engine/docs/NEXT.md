# Roadmap

## Milestone 2: NDI outputs on the Windows machine

- [x] GPU UYVY 4:2:2 packing, now the default: readback halved (7.96 → 3.98 GB/s at 4×4K), accuracy verified
- [x] Precise frame pacing and real-time / MMCSS thread priority: tick lateness 3–4 ms → 0.001 ms on macOS
- [x] `tools/bench.py` (one-command benchmark) and `tools/check_uyvy.py`
- [x] Windows CI: MSVC build plus Direct3D 11 pixel checks on WARP on every push
- [x] First run on the Windows Mini PC (AMD Radeon 610M) with the NDI 6 SDK: 4 NDI outputs
      and an NDI input, 2 hours, no tearing seen, streams play in NDI Studio Monitor
- [ ] 60 fps on that Mini PC. The first run was GPU-bound at about 30 fps (99 % GPU, 29.7 ms per
      frame). The changes below cut GPU work for that case; re-measure with `python tools\bench.py`.
- [ ] 4 × 4K60 on a machine with a discrete GPU, for 30 minutes
- [ ] Glass-to-glass latency measurement

### GPU work removed after the first Windows run

On an integrated GPU, memory bandwidth is the limit, so each full-screen pass matters:

| Change | Saves per frame |
|---|---|
| Single-pass UYVY: outputs are mapped, sampled and packed in one shader; no full-size BGRA copy | 1 full-resolution write + read per output |
| Direct path: one opaque source exactly filling the canvas is sampled by the outputs directly | the whole canvas pass (33 MB written and read at 4K) |
| Hidden / disabled / transparent sources are not drawn; a frozen test pattern is drawn once | the test pattern pass (ALU-heavy) |
| Blending only for translucent draws | a read of the target on every opaque draw |

On the M4, 24 × 1080p went from 21.4 ms to 16.1 ms of GPU time.

## Milestone 3: 20–24 streams

- [x] Wall tool: rows × columns of panels with bezel gaps, generating the outputs
- [x] 24 × 1080p at 60 fps with no drops (Apple M4, null sink): see the README table
- [x] Example 24-panel layout, `layouts/wall-6x4-1080p.json`
- [ ] 24 outputs with real NDI senders. Watch CPU, `sendMs`/`sendMsMax` and `droppedQueue`.
      If NDI encoding on 24 sender threads oversubscribes the cores, move the senders to a
      pool with one queue per output.
- [ ] NDI multicast for many receivers (NDI Access Manager), instead of a TCP stream per receiver
- [ ] Batched readback (one staging atlas per frame) if per-output map/unmap shows up in profiles

Why each item is planned this way:
- **Bandwidth.** 24 × 1080p60 UYVY is about 6 GB/s of readback. 24 × 4K60 isn't realistic on one
  machine; plan on 1080p per display, or several engine instances each owning part of the canvas.
- **Sender threads.** CPU per frame is 1.1 ms at 24 outputs with the null sink, so the thread-per-output
  design isn't the bottleneck yet. The NDI encode cost per stream will decide whether a pool is needed.
- **Multi-instance.** If outputs are split across machines, share the layout through the
  REST API and align on NDI timecode.

## Possible next features

- Video file and still image sources
- Edge blending and simple keystone / warping for projectors
- Per-output colour adjustment (brightness / contrast) to match panels
- Scene triggers over the REST API from a show controller (already possible with curl; a small OSC bridge would make it easier)
- Audio passthrough from an NDI input to one output
