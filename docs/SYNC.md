# Synchronisation: three different things

"Are the four screens in sync?" covers three separate layers. This engine controls only the first.

## 1. Synchronised frame generation (guaranteed by the engine)

Each tick, the render thread:
1. updates every source
2. composes the canvas once
3. renders every enabled output from that same canvas
4. commits it all in **one GPU submission**

Every output of that tick carries the same engine `frameId` and the same NDI `timecode`. The frame id is also:
- drawn in the test pattern, as digits and a binary strip
- sent as NDI metadata: `<mosaic frame_id="N"/>`

How it's measured:
- **frames on all outputs** (`sync.completeRatio` in `/api/metrics`): the share of
  rendered frames that every enabled output actually handed to its sender. A frame
  dropped on one output only (for example, its sender fell behind) counts as *partial*.
- **lastFrameIdSpread**: the difference between the newest frame ids the outputs have
  sent. It's 0 when the outputs keep pace, though it can briefly read 1 because it's
  sampled while frames are moving.

In the measured runs every frame was complete (1569/1569 at 4×4K).

## 2. NDI transport timing (not controlled)

Each output is an independent NDI sender. NDI encodes on its own threads and sends
over TCP/UDP. Frames that leave this engine together can reach the receivers at
slightly different times, depending on:
- encode load
- network queueing
- receiver buffering

The engine sets identical timecodes, so a receiver that buffers by timecode *can*
align them. Most receivers simply display frames as they arrive.

## 3. Physical display synchronisation (not controlled)

Even with perfectly aligned frames, each receiving PC presents on its own display
clock (vsync), and each display adds its own processing delay. Locking those
together needs hardware or driver features, which this project doesn't touch:
- genlock / framelock (NVIDIA Quadro Sync, AMD FirePro S400)
- displays driven from one GPU with a mosaic or Eyefinity group
- receivers that support PTP/timecode-locked presentation

**Practical guidance:** for a tear-free wall, drive the displays from one receiving
machine and GPU where possible. That's your current setup, with NDI Studio Monitor
on one Mini PC. Use the frame-id strip in the test pattern, filmed with a phone in
slow motion, to measure the real offset between screens.
