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
3. `GET /api/metrics` (canvas header `X-Mosaic-Canvas: canvas-N`) → that output: `open: true`,
   `fps` ≈ the refresh rate (or half for a 30 fps canvas), `busy` and `repeats` not climbing fast,
   no `error`.
4. Windows' display settings: each wall's monitors must be **extended** (not duplicated), at the
   resolution you want; the output is drawn at whatever mode the monitor is in.

## 5. Capture card

1. Close OBS (a card opens in one program at a time).
2. Inputs → **+ Add… → Capture card** → pick the card under *Device*. `GET /api/capture` lists the
   devices; the built-in *Mosaic test capture* is always there for comparison.
3. **Send as NDI stream** in its panel → check it in Studio Monitor or on a receiver.
4. If the card shows an error: send it to me (the text says whether Windows can't convert its format,
   or another program has it).

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

## Going back

If something's wrong and the venue is about to open: tell me, I revert and push, then
**Update everything** again. Or stop a new canvas (**Stop canvas**) and set outputs back to NDI;
the first canvas works as before.
