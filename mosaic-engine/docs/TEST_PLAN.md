# Test plan: canvases, GPUs, display outputs, capture, colour (venue, after hours)

Everything below was self-tested on a Mac (Metal) and syntax-checked for Windows with MinGW
(`tools/wincheck.sh`), but the Windows-only parts have never run on Windows. Do it after hours:
an engine update blanks the wall for a minute or two.

## 0. Update

1. Editor → Settings → Updates → **Update everything**.
2. The result line should say `Up to date: <commit>`. If it says *finished with errors*, run on
   PH-MEDIA and send the output (the venue keeps running the previous engine meanwhile):

   ```powershell
   Select-String -Path C:\Users\gary\Documents\video-wall-control\logs\update.log -Pattern ': error|LNK[0-9]|Fail' | ForEach-Object { $_.Line } | Sort-Object -Unique | Select-Object -First 40
   ```

3. Nothing at the venue changes by itself: the current layout is canvas 1, outputs stay NDI.

## 1. Receiver fix (tear between displays)

Endpoint panel of DESKTOP-K6NIVJ9, or `GET /api/endpoints` → `receiver`:

| | Before | Expect |
|---|---|---|
| `presentFps` | ~33 | ~60 |
| `windowPresentsPerS` | – | ~120 (30 fps content) |
| `compensationChanges` | – | 2, and stays there |
| `screenSpreadNowMs` | 14–16 | ~0 |
| `freezesLastMinute` | 32 | a few at most |

Look at the wall: the top and bottom rows should change on the same refresh. Film a fast pan in slow
motion if unsure.

## 2. Canvases

1. Top bar → **+ Canvas** → "Mosaic 2". The editor switches to it.
2. Screens → **+ Group** → 2×2 (or whatever Mosaic 2 is). Its NDI names are `Mosaic_2_01`…
3. Endpoints (on Mosaic 2) → set up the 4 receiving PCs, player Mosaic Receiver, Buffer ~300 ms.
4. Switch back to the first canvas with the **Canvas** picker: it's unchanged and still on air.

## 3. GPU per canvas

1. `GET http://192.168.1.187:8090/api/gpus` should list the 3 GPUs and the monitors on each.
2. Settings → **Canvas** → GPU: pick one for each wall's canvas. The canvas restarts on it
   (`/api/canvases` → `gpu`).

## 4. Display outputs (video walls 1 and 2)

1. A canvas per wall (for example "Video wall 1"), on the GPU its DisplayPort cables are on.
2. Its outputs → **Send to** → the monitor (the list shows the monitors of the canvas's GPU).
   For a group: its *Displays* tab → **Send to monitors in order** (row by row, to the monitors left to
   right, top to bottom as Windows arranges them).
3. `GET /api/metrics?canvas=canvas-N` → that output: `open: true`,
   `fps` ≈ the refresh rate (or half for a 30 fps canvas), `busy` and `repeats` not climbing fast,
   no `error`.
4. Windows' display settings: each wall's monitors must be **extended** (not duplicated). The output is
   drawn at whatever mode the monitor is in; to change it, pick one under the output's **Monitor →
   Resolution** (the modes the monitor offers; the engine sets it in Windows and reopens the output).
5. **Delay** (same section): a display output shows a frame almost at once, a wall fed over NDI a few
   hundred ms later. Raise the delay on the display output until both show the same frame
   (`/api/metrics` → `heldFrames`).

## 5. Capture card

1. Close OBS (a card opens in one program at a time).
2. Inputs → **+ Add… → Capture card** → pick the card under *Device*. `GET /api/capture` lists the
   devices; the built-in *Mosaic test capture* is always there for comparison.
3. **Send as NDI stream** in its panel → check it in Studio Monitor or on a receiver.
4. If the card shows an error: send it to me (the text says whether Windows can't convert its format,
   or another program has it).
5. Its sound: Settings → Audio → **+ Output** → Source → *Sound inputs* → the card's recording
   device. Clap in front of the camera and adjust the trim until sound and picture line up.

## 6. Colour correction

Select an output → **Colour** → nudge red / brightness and watch that display. Double-click a slider
resets it; *Reset colour* resets all.

## 7. 8K AV1 clip (software decoding)

1. `GET /api/system/decoders`: is there an **AV1** decoder with `hardware: true`? If not, the AV1
   Video Extension from the Microsoft Store (free) or the NVIDIA driver's AV1 support is missing.
2. The video source's status line has a tooltip (`decoder`) saying what it tried and why it fell back.

## 8. Several receiving PCs on one canvas

With Mosaic 2's PCs running, the Endpoints panel shows "*N* PCs in step: *x* ms apart". Expect under
a refresh (17 ms) with a Buffer set; hovering shows each PC's offset.

## 9. A canvas recovering by itself

`POST /api/canvases/canvas-N/simulate-gpu-loss` (not on the canvas that's on air): it stops and
starts again within a few seconds (`GET /api/canvases` → `restarts`), the other canvases untouched.

## 10. One stream across a 2×2 (inside bar centre)

A 2×2 of screens on one receiving PC can be fed by **one** stream that the Mosaic Receiver cuts into
quarters: the four screens always show pieces of the same frame, and the media server renders, reads
back and encodes one stream instead of four.

1. Update **both** PCs (Update everything): the receiving PC's dashboard has to pass the quarters on.
2. The canvas needs one output covering the whole 2×2 (the layout **Inside bar** has it: *Centre 2x2*,
   3840×2160).
3. Endpoints → the PC → remove its four displays → **+ 2×2 from one output…** → that output. Each
   display row now says *Shows: top left of 2×2* and so on; set each row's **Screen**.
4. Player must be **Mosaic Receiver** (Studio Monitor can't show part of a stream; the row says so).
5. Expect on the PC's panel: 4 displays playing the same stream, *apart* ≈ 0 ms, and on the media
   server one `Inside_Bar_01` stream with 1 connection instead of four streams.
6. **Bezel correction:** each of those display rows has **Bezel gap across / down (px)**: the gap
   from the edge of one picture to the edge of the next (both bezels), in pixels of the stream.
   Setting it on one row sets it for the whole 2×2. That much of the picture is left out between
   the screens, so a diagonal line or a ticker runs straight from one screen onto the next. On
   the Inside bar layout 1 mm is about 0.9 px (10 mm of bezel between pictures = 9 px). Show the
   test pattern and raise the gap until its diagonals line up across the seams.

## 11. Inside bar layout

Layouts → **Inside bar - 2x2 of 98" with 40" and 24" side screens** (run it on the inside bar's
canvas). 11 outputs: the centre 2×2 as one stream, and five screens a side placed from the wall's
measurements (`tools/make_inside_bar.py`; change the inches there and run it again). A tilted or
portrait screen whose picture is upside down: add 180 to its *angle* (or set rotation 270).

## Going back

If something's wrong and the venue is about to open: tell me, I revert and push, then
**Update everything** again. Or stop a new canvas (**Stop canvas**) and set outputs back to NDI;
the first canvas works as before.
