# Next milestone: 4 × 4K60 over NDI, then 20–24 streams

## Milestone 2: four 4K60 NDI outputs on the Windows workstation

1. **First Windows run** (see WINDOWS.md): Direct3D 11.4 with the null sink, 4 × 1080p
   then 4 × 4K, recording `renderFps`, `gpuMs`, `latencyMs` and drops.
   The aim is to confirm 33 MB/frame/output of PCIe readback is sustainable.
2. **Enable NDI** and repeat. NDI encode cost will dominate: watch `sendMs`/`sendMsMax`,
   `droppedQueue`, and CPU usage.
3. **GPU UYVY conversion.** Add an output pass that writes UYVY 4:2:2 (BT.709) into a
   half-width render target, and send it as `NDIlib_FourCC_video_type_UYVY`. This halves
   readback and NDI input bandwidth. NDI's native format is UYVY, so this also removes
   NDI's own BGRA→UYVY conversion. This is the single biggest win.
4. **Pin sender threads** and raise their priority. Set the render thread to MMCSS "Games"/"Pro Audio" class.
5. **Measure end to end.** Film the receiver screens next to the engine's frame
   counter, or use the binary strip with a capture card, to get glass-to-glass latency.

Acceptance: 4 × 3840×2160 at 60 fps for 30 minutes, 0 partial frames, no growth
in queue depth, and the four streams playing in NDI Studio Monitor on a separate machine.

## Milestone 3: 20–24 streams

The architecture already scales by output count: one render target, readback ring
and sender thread per output, all from one canvas pass. What changes at 20+:

- **Bandwidth budget.** 24 × 1080p60 UYVY ≈ 6 GB/s of readback; 24 × 4K60 isn't
  realistic on one machine. Plan on 1080p per display, or several engine instances
  (each owning part of the canvas), all fed the same NDI source.
- **Sender threads → thread pool.** 24 threads is fine, but a pool with one
  queue per output avoids oversubscribing the cores during NDI encoding.
- **Batch readbacks.** Copy many outputs into one large staging texture per frame
  (atlas), so there's one map per frame instead of N.
- **NDI multicast.** For many receivers, enable multicast in NDI Access Manager to
  avoid per-receiver TCP streams.
- **Multi-instance coordination.** If outputs are split across machines, share the
  layout through the REST API and align on NDI timecode.
- **Editor.** Group outputs into "screens" and add snapping to neighbouring edges
  and bezel compensation (gaps between panels), which matter far more at 24 displays.
