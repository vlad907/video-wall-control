#!/usr/bin/env python3
"""End-to-end check of a built engine, used by CI (Windows/WARP) and locally.

Runs the engine on a frozen test pattern and checks real pixels:
  - every output's corner markers have the right colours in the right corners
  - a 90-degree output has its corners rotated clockwise
  - a free angle tilts the sampled region about its centre
  - PATCHing an output's crop over the REST API changes what it renders
  - a half-transparent source blends over the one below it
  - scenes recall, and crossfade when asked to
  - the wall tool generates outputs with bezel gaps and resizes the canvas
  - wall groups: a second (1x1) group, moving it, deleting it with its outputs
  - group presets: save, clear, recall with a crossfade limited to the group
  - image sources, the signal-loss fallback image, and layout history / restore
  - a video file: plays, pauses, restarts and loops (skipped where Media Foundation is missing)
  - endpoints: check-in, assignment and status of a receiving PC
  - every rendered frame reached every output (no partial frames)

    python tools/ci_check.py [--engine PATH] [--out DIR]

Set MOSAIC_SOFTWARE_GPU=1 to run on WARP (no GPU needed). Exits non-zero on failure.
"""
import argparse, json, os, subprocess, sys, tempfile, time, urllib.error, urllib.request

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
default_engine = os.path.join(root, "build", "Release", "mosaic_engine.exe") if os.name == "nt" else os.path.join(root, "build", "mosaic_engine")
ap = argparse.ArgumentParser()
ap.add_argument("--engine", default=default_engine)
ap.add_argument("--out", default="ci-out")
ap.add_argument("--port", type=int, default=8098)
args = ap.parse_args()
os.makedirs(args.out, exist_ok=True)
BASE = f"http://127.0.0.1:{args.port}"

RED, GREEN, BLUE, WHITE = (255, 0, 0), (0, 255, 0), (26, 89, 255), (255, 255, 255)

def out(i, crop, **kw):
    o = {"id": f"out-{i}", "name": f"Out {i}", "width": 1920, "height": 1080,
         "crop": dict(zip("xywh", crop)), "pixelFormat": "uyvy"}
    o.update(kw)
    return o

layout = {
    "id": "ci", "name": "CI check", "frameRate": 60,
    "canvas": {"width": 3840, "height": 2160, "background": [0, 0, 0, 1]},
    "sources": [{"id": "pattern", "type": "test_pattern", "animated": False,
                 "rect": {"x": 0, "y": 0, "w": 3840, "h": 2160}}],
    "outputs": [
        out(1, (0, 0, 1920, 1080)),
        out(2, (1920, 0, 1920, 1080)),
        out(3, (0, 1080, 1920, 1080), pixelFormat="bgra"),
        out(4, (1920, 1080, 1920, 1080)),
        # tile 1 rotated 90 clockwise, scaled to fit a 1920x1080 output
        out(5, (0, 0, 1920, 1080), rotation=90, scaleMode="stretch"),
    ],
}

failures = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg, flush=True)
    if not cond:
        failures.append(msg)

def get(path, timeout=10, canvas=None):
    req = urllib.request.Request(BASE + path, headers={"X-Mosaic-Canvas": canvas} if canvas else {})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.headers, r.read()

def call(method, path, body=None, canvas=None):
    data = json.dumps(body).encode() if body is not None else b"{}"
    headers = {"Content-Type": "application/json"}
    if canvas:
        headers["X-Mosaic-Canvas"] = canvas
    req = urllib.request.Request(BASE + path, method=method, data=data, headers=headers)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read() or b"null")

def frame(oid, canvas=None):
    for _ in range(30):  # WARP can be slow to produce the first frames
        try:
            h, body = get(f"/api/outputs/{oid}/frame.bgra", canvas=canvas)
            return int(h["X-Width"]), int(h["X-Height"]), body
        except Exception:
            time.sleep(0.5)
    raise RuntimeError(f"no frame from {oid}")

def pixel(img, x, y):
    w, _, data = img
    i = (y * w + x) * 4
    return data[i + 2], data[i + 1], data[i]  # BGRA -> RGB

def close(a, b, tol=48):
    return all(abs(p - q) <= tol for p, q in zip(a, b))

def png_bytes(w, h, rgb):
    """A small solid-colour PNG, written by hand (no imaging library needed)."""
    import struct, zlib
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    rows = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))

def corners(img):
    w, h, _ = img
    m = 60  # centre of the 96 px markers inset 12 px
    return {"TL": pixel(img, m, m), "TR": pixel(img, w - m, m), "BL": pixel(img, m, h - m), "BR": pixel(img, w - m, h - m)}

with tempfile.TemporaryDirectory() as tmp:
    os.makedirs(f"{tmp}/layouts")
    json.dump(layout, open(f"{tmp}/layouts/ci.json", "w"))
    log = open(os.path.join(args.out, "engine.log"), "w")
    proc = subprocess.Popen([args.engine, "--data", f"{tmp}/data", "--layouts", f"{tmp}/layouts", "--layout", "ci",
                             "--port", str(args.port), "--ui", os.path.join(root, "ui", "dist")], stderr=log, stdout=log)
    try:
        for _ in range(60):
            try:
                get("/api/health", timeout=2)
                break
            except Exception:
                time.sleep(0.5)
        print("engine:", flush=True)
        time.sleep(3)
        m = json.loads(get("/api/metrics")[1])
        e = m["engine"]
        print(f"  {e['backend']} on {e['device']}, {e['renderFps']:.1f} fps", flush=True)

        print("corner markers (orientation and crop):", flush=True)
        expected = {"TL": RED, "TR": GREEN, "BL": BLUE, "BR": WHITE}
        for i in (1, 2, 3, 4):
            img = frame(f"out-{i}")
            check(img[0] == 1920 and img[1] == 1080, f"out-{i} is 1920x1080 (got {img[0]}x{img[1]})")
            c = corners(img)
            for k, want in expected.items():
                check(close(c[k], want), f"out-{i} {k} marker {c[k]} ~ {want}")

        print("rotation 90 clockwise:", flush=True)
        c = corners(frame("out-5"))
        # Clockwise: the source's bottom-left becomes the top-left, top-left becomes top-right.
        for k, want in {"TL": BLUE, "TR": RED, "BR": GREEN, "BL": WHITE}.items():
            check(close(c[k], want), f"out-5 {k} marker {c[k]} ~ {want}")

        print("free angle (tilted panels):", flush=True)
        centre = (960, 540)
        straight = pixel(frame("out-1"), *centre)
        call("PATCH", "/api/outputs/out-1", {"angle": 180})
        time.sleep(1.0)
        c = corners(frame("out-1"))
        for k, want in {"TL": WHITE, "TR": BLUE, "BL": GREEN, "BR": RED}.items():
            check(close(c[k], want), f"angle 180: out-1 {k} marker {c[k]} ~ {want}")
        call("PATCH", "/api/outputs/out-1", {"angle": 12.5})
        time.sleep(1.0)
        img = frame("out-1")
        check(close(pixel(img, *centre), straight, 30), f"angle 12.5: centre pixel stays {pixel(img, *centre)} ~ {straight}")
        check(not close(pixel(img, 60, 60), RED, 48), "angle 12.5: the corner marker moved away from the corner")
        call("PATCH", "/api/outputs/out-1", {"angle": 0})
        time.sleep(1.0)

        print("live crop change over REST:", flush=True)
        probe = [(330, 330), (1500, 330), (330, 800), (1500, 800)]  # inside grid cells: the tile's own colour
        before = [pixel(frame("out-1"), x, y) for x, y in probe]
        target = [pixel(frame("out-2"), x, y) for x, y in probe]
        check(any(not close(a, b, 10) for a, b in zip(before, target)), "out-1 and out-2 differ before the change")
        req = urllib.request.Request(BASE + "/api/outputs/out-1", method="PATCH",
                                     data=json.dumps({"crop": {"x": 1920, "y": 0}}).encode(),
                                     headers={"Content-Type": "application/json"})
        urllib.request.urlopen(req, timeout=10).read()
        time.sleep(1.5)
        after = [pixel(frame("out-1"), x, y) for x, y in probe]
        check(all(close(a, b, 10) for a, b in zip(after, target)), f"out-1 now matches out-2 {after} ~ {target}")

        print("source opacity (alpha blending):", flush=True)
        spot = (330, 330)
        base = pixel(frame("out-1"), *spot)
        red = call("POST", "/api/sources", {"name": "red", "type": "solid", "color": [1, 0, 0, 1], "opacity": 0.5})
        time.sleep(1.0)
        got = pixel(frame("out-1"), *spot)
        want = tuple(round(0.5 * a + 0.5 * b) for a, b in zip(base, (255, 0, 0)))
        check(close(got, want, 12), f"50% red over the pattern {got} ~ {want}")
        call("DELETE", f"/api/sources/{red['id']}")

        print("scenes:", flush=True)
        call("POST", "/api/scenes", {"name": "pattern only"})
        call("POST", "/api/sources", {"name": "blue", "type": "solid", "color": [0, 0, 1, 1]})
        call("POST", "/api/scenes", {"name": "all blue"})
        time.sleep(1.0)
        check(close(pixel(frame("out-1"), *spot), (0, 0, 255), 12), "blue source covers the canvas")
        call("POST", "/api/scenes/pattern-only/recall")
        time.sleep(1.0)
        check(close(pixel(frame("out-1"), *spot), base, 12), "recalling 'pattern only' restores the pattern")
        call("POST", "/api/scenes/all-blue/recall", {"transitionMs": 4000})
        time.sleep(1.2)
        mid = pixel(frame("out-1"), *spot)
        check(not close(mid, base, 20) and not close(mid, (0, 0, 255), 20), f"mid-crossfade is a mix {mid}")
        time.sleep(4.0)
        check(close(pixel(frame("out-1"), *spot), (0, 0, 255), 12), "crossfade ends on the new scene")
        call("POST", "/api/scenes/pattern-only/recall")

        print("wall tool (bezel gaps):", flush=True)
        lay = call("PUT", "/api/wall", {"cols": 2, "rows": 1, "outputWidth": 1920, "outputHeight": 1080, "gapX": 100, "fitCanvas": True})
        crops = {o["id"]: o["crop"] for o in lay["outputs"]}
        # 3940 wide for the wall; 2160 high because standalone out-3/out-4 sit below it.
        check(lay["canvas"]["width"] == 3940 and lay["canvas"]["height"] == 2160, f"canvas fits the wall and the other outputs ({lay['canvas']['width']}x{lay['canvas']['height']})")
        check(crops["out-1"]["x"] == 0 and crops["out-2"]["x"] == 2020, f"panel 2 starts after the 100 px bezel (x={crops['out-2']['x']})")
        check(all(o["group"] == "wall" for o in lay["outputs"] if o["id"] in ("out-1", "out-2")), "wall outputs are grouped")
        check(len(lay["outputs"]) == 5, f"other outputs kept ({len(lay['outputs'])} outputs)")
        check(lay["sources"][0]["rect"]["w"] == 3940, "full-canvas source follows the new canvas")
        time.sleep(1.5)
        img = frame("out-2")
        check(img[0] == 1920 and img[1] == 1080, "wall output renders")

        print("wall groups:", flush=True)
        lay = call("POST", "/api/walls", {"name": "Double wide", "cols": 1, "rows": 1, "outputWidth": 3840, "outputHeight": 1080, "y": 1080})
        dw = [o for o in lay["outputs"] if o.get("group") == "double-wide"]
        check(len(dw) == 1 and dw[0]["id"] == "double-wide-1" and dw[0]["name"] == "Double wide", f"1x1 group makes one output ({[o['id'] for o in dw]})")
        ndi = [o["ndiName"] for o in lay["outputs"]]
        check(len(set(ndi)) == len(ndi), "every output keeps a unique NDI name")
        check(lay["canvas"]["width"] == 3940 and lay["canvas"]["height"] == 2160, "a new group leaves the canvas alone")
        check(len(lay["walls"]) == 2 and any(o.get("group") == "wall" for o in lay["outputs"]), "the first wall is still there")
        lay = call("POST", "/api/walls/double-wide/move", {"x": 50, "y": 0})
        crop = next(o["crop"] for o in lay["outputs"] if o["id"] == "double-wide-1")
        check(crop["x"] == 50 and crop["y"] == 0 and crop["w"] == 3840, f"moving a group shifts its outputs ({crop})")
        img = frame("double-wide-1")
        check(img[0] == 3840 and img[1] == 1080, "group output renders")
        call("DELETE", "/api/walls/double-wide?outputs=delete")
        lay = json.loads(get("/api/layout")[1])
        check(not any(o.get("group") == "double-wide" for o in lay["outputs"]) and len(lay["walls"]) == 1, "deleting a group removes its outputs")

        print("group presets:", flush=True)
        call("POST", "/api/walls", {"name": "Side", "cols": 1, "rows": 1, "x": 0, "y": 1080})
        time.sleep(1.0)
        side0, other0 = pixel(frame("side-1"), *spot), pixel(frame("out-2"), *spot)
        call("POST", "/api/sources", {"name": "Blue", "type": "solid", "color": [0, 0, 1, 1], "group": "side",
                                      "rect": {"x": 0, "y": 1080, "w": 1920, "h": 1080}})
        saved = call("POST", "/api/walls/side/presets", {"name": "All blue"})
        check(saved["id"] == "all-blue" and saved["sources"][0]["rect"]["y"] == 0, "preset saved relative to the group")
        call("POST", "/api/walls/side/clear")
        time.sleep(1.0)
        check(close(pixel(frame("side-1"), *spot), side0, 12), "clearing the group shows what's under it")
        call("POST", "/api/walls/side/presets/all-blue/recall", {"transitionMs": 4000})
        time.sleep(1.2)
        mid = pixel(frame("side-1"), *spot)
        check(not close(mid, side0, 20) and not close(mid, (0, 0, 255), 20), f"the group crossfades {mid}")
        check(close(pixel(frame("out-2"), *spot), other0, 12), "outputs outside the group don't fade")
        time.sleep(3.5)
        check(close(pixel(frame("side-1"), *spot), (0, 0, 255), 12), "recalled preset fills the group")
        ctl = json.loads(get("/api/control")[1])
        g = next(x for x in ctl["groups"] if x["id"] == "side")
        check(g["activePreset"] == "all-blue" and [p["id"] for p in g["presets"]] == ["all-blue"], "control summary lists the group's presets")
        lay = call("POST", "/api/walls/side/move", {"x": 2020, "y": 1080})
        rect = next(x["rect"] for x in lay["sources"] if x.get("group") == "side")
        check(rect["x"] == 2020 and rect["y"] == 1080, "group content moves with the group")
        call("DELETE", "/api/walls/side?outputs=delete")

        print("images and signal-loss fallback:", flush=True)
        png = png_bytes(8, 8, (255, 200, 0))
        req = urllib.request.Request(BASE + "/api/media?name=slate.png", method="POST", data=png, headers={"Content-Type": "image/png"})
        with urllib.request.urlopen(req, timeout=10) as r:
            check(json.loads(r.read())["name"] == "slate.png", "image uploaded")
        check(any(f["name"] == "slate.png" for f in json.loads(get("/api/media")[1])["files"]), "image listed")
        img_src = call("POST", "/api/sources", {"name": "Slate", "type": "image", "image": "slate.png", "rect": {"x": 0, "y": 0, "w": 1920, "h": 1080}})
        time.sleep(1.0)
        check(close(pixel(frame("out-1"), *spot), (255, 200, 0), 12), "image source draws")
        call("DELETE", f"/api/sources/{img_src['id']}")
        call("PUT", "/api/layout", {"fallbackImage": "slate.png"})
        ndi_src = call("POST", "/api/sources", {"name": "Gone", "type": "ndi", "ndiName": "NOWHERE (Nothing)", "rect": {"x": 0, "y": 0, "w": 1920, "h": 1080}})
        time.sleep(4.5)
        check(close(pixel(frame("out-1"), *spot), (255, 200, 0), 12), "a stream with no signal shows the fallback image")
        st = next(x for x in json.loads(get("/api/sources")[1])["sources"] if x["id"] == ndi_src["id"])["status"]
        check(st.get("signal") == "lost" and st.get("showing") == "fallback image", f"status reports the lost signal ({st.get('signal')}, {st.get('showing')})")
        call("DELETE", f"/api/sources/{ndi_src['id']}")
        call("PUT", "/api/layout", {"fallbackImage": ""})

        print("video file:", flush=True)
        clip = open(os.path.join(here, "test-clip.mp4"), "rb").read()  # 1 s red, then 1 s blue (H.264, 30 fps)
        req = urllib.request.Request(BASE + "/api/media?name=clip.mp4", method="POST", data=clip, headers={"Content-Type": "video/mp4"})
        with urllib.request.urlopen(req, timeout=20) as r:
            check(json.loads(r.read())["kind"] == "video", "video uploaded")
        vid = call("POST", "/api/sources", {"name": "Clip", "type": "video", "video": "clip.mp4", "loop": True,
                                            "rect": {"x": 0, "y": 0, "w": 1920, "h": 1080}})
        def vstatus():
            return next(x for x in json.loads(get("/api/sources")[1])["sources"] if x["id"] == vid["id"])["status"]
        time.sleep(0.6)
        st = vstatus()
        print(f"  ..   {st.get('status')} · {st.get('video')}", flush=True)
        if "Media Foundation" in st.get("status", "") or "no decoder" in st.get("status", ""):
            print("  skip this machine can't decode H.264 (no Media Foundation): video checks skipped", flush=True)
        else:
            check(st.get("video", {}).get("codec") == "H.264" and st["video"]["width"] == 320, "video opened (H.264, 320x180)")
            call("POST", f"/api/sources/{vid['id']}/restart", {})
            time.sleep(0.4)
            check(close(pixel(frame("out-1"), *spot), (255, 0, 0), 40), f"plays the first second (red) {pixel(frame('out-1'), *spot)}")
            time.sleep(1.0)
            check(close(pixel(frame("out-1"), *spot), (0, 0, 255), 40), f"then the next (blue) {pixel(frame('out-1'), *spot)}")
            call("PATCH", f"/api/sources/{vid['id']}", {"playing": False})
            time.sleep(0.3)
            held = pixel(frame("out-1"), *spot)
            time.sleep(1.2)
            check(close(pixel(frame("out-1"), *spot), held, 12) and vstatus()["status"] == "paused", "pausing holds the picture")
            call("PATCH", f"/api/sources/{vid['id']}", {"playing": True})
            call("POST", f"/api/sources/{vid['id']}/restart", {})
            time.sleep(0.4)
            check(close(pixel(frame("out-1"), *spot), (255, 0, 0), 40), "restart goes back to the first frame (red)")
            time.sleep(2.2)
            check(vstatus()["status"] == "playing", "it loops (still playing after the end)")
        call("DELETE", f"/api/sources/{vid['id']}")
        call("DELETE", "/api/media/clip.mp4")

        print("layout history:", flush=True)
        hist = json.loads(get("/api/layouts/ci/history")[1])["history"]
        check(len(hist) >= 1, f"earlier versions are kept ({len(hist)})")
        names_before = [o["name"] for o in json.loads(get("/api/layout")[1])["outputs"]]
        call("POST", f"/api/layouts/ci/history/{hist[-1]['version']}/restore", {})
        restored = json.loads(get("/api/layout")[1])
        check(len(restored["outputs"]) == 5 and not restored["walls"], "restoring the oldest version brings back the original layout")
        check(len(json.loads(get("/api/layouts/ci/history")[1])["history"]) == len(hist) + 1, "the replaced version is kept too")

        print("endpoints:", flush=True)
        r = call("POST", "/api/endpoints/checkin", {"host": "WALL-PC.example.org", "screens": [], "panels": []})
        check(r["endpoint"] is None, "an unknown PC gets no assignment")
        unassigned = [u["host"] for u in json.loads(get("/api/endpoints")[1])["unassigned"]]
        check("WALL-PC.example.org" in unassigned, "it is listed as a PC to set up")
        call("POST", "/api/endpoints", {"name": "Spare PC", "host": "spare-pc", "displays": [{"output": "out-1"}]})
        spare = next(x for x in json.loads(get("/api/endpoints")[1])["endpoints"] if x["id"] == "spare-pc")
        check(not spare["online"] and spare["lastSeenSecondsAgo"] is None, "an endpoint that never checked in is listed offline")
        call("POST", "/api/endpoints", {"name": "Wall PC", "host": "wall-pc", "displays": [{"output": "out-1"}, {"output": "out-2"}]})
        r = call("POST", "/api/endpoints/checkin", {"host": "WALL-PC.example.org", "screens": [],
                                                    "panels": [{"index": 1, "reachable": True, "playing": "X"}]})
        streams = [d.get("stream", "") for d in r["displays"]]
        check(r["endpoint"]["id"] == "wall-pc" and len(streams) == 2 and streams[0].endswith("(Mosaic_Output_01)"),
              f"its check-in returns its displays ({streams})")
        e = next(x for x in json.loads(get("/api/endpoints")[1])["endpoints"] if x["id"] == "wall-pc")
        check(e["online"] and not e["displays"][0]["ok"], "status shows it online, display 1 playing the wrong stream")
        call("PUT", "/api/endpoints/wall-pc", {"player": "mosaic"})
        r = call("POST", "/api/endpoints/checkin", {"host": "wall-pc", "panels": [], "receiver": {"spreadMax10s": 0, "alignedRatio": 1.0, "refreshHz": 60,
                                                    "displays": [{"index": 1, "shownFrameId": 1234, "late": 2}]}})
        check(r["endpoint"]["player"] == "mosaic", "the check-in tells the PC to use the Mosaic Receiver")
        e = next(x for x in json.loads(get("/api/endpoints")[1])["endpoints"] if x["id"] == "wall-pc")
        check(e["receiver"]["spreadMax10s"] == 0 and e["displays"][0]["shownFrameId"] == 1234, "the receiver's sync status reaches the editor")

        print("colour correction:", flush=True)
        def mean_rgb(img):
            w, h, data = img
            n, acc = 0, [0, 0, 0]
            for i in range(0, len(data), 4 * 101):
                acc[0] += data[i + 2]; acc[1] += data[i + 1]; acc[2] += data[i]; n += 1
            return [a / n for a in acc]
        call("PATCH", "/api/outputs/out-1", {"color": None})
        time.sleep(0.5)
        base_rgb = mean_rgb(frame("out-1"))
        call("PATCH", "/api/outputs/out-1", {"color": {"red": 0.5}})
        time.sleep(0.5)
        half = mean_rgb(frame("out-1"))
        check(abs(half[0] - base_rgb[0] * 0.5) < 12 and abs(half[1] - base_rgb[1]) < 12, f"red gain 0.5 halves red only ({base_rgb[0]:.0f} -> {half[0]:.0f})")
        call("PATCH", "/api/outputs/out-1", {"color": {"red": 1, "saturation": 0}})
        time.sleep(0.5)
        g = mean_rgb(frame("out-1"))
        check(max(g) - min(g) < 3, f"saturation 0 makes it grey ({[round(x) for x in g]})")
        call("PATCH", "/api/outputs/out-1", {"color": None})
        check("color" not in next(o for o in json.loads(get("/api/layout")[1])["outputs"] if o["id"] == "out-1"), "reset leaves no colour settings")

        print("capture card (built-in test device):", flush=True)
        devs = call("GET", "/api/capture")["devices"]
        check(any(d["kind"] == "test" for d in devs), f"capture devices listed ({[d['name'] for d in devs]})")
        call("POST", "/api/sources", {"id": "cap", "name": "Capture", "type": "capture", "device": "Mosaic test capture", "rect": {"x": 0, "y": 0, "w": 64, "h": 36}})
        call("POST", "/api/outputs", {"id": "cap-ndi", "name": "Capture (NDI)", "source": "cap", "crop": {"x": 0, "y": 0, "w": 1280, "h": 720}, "width": 1280, "height": 720, "pixelFormat": "bgra"})
        time.sleep(1.5)
        img = frame("cap-ndi")
        bars = [pixel(img, x, 360) for x in range(80, 1280, 160)]
        check(close(bars[0], (255, 255, 255), 30) and close(bars[1], (255, 255, 0), 30) and close(bars[6], (0, 0, 255), 30), f"a capture source sent as NDI carries its picture ({bars[:3]}...)")
        st = next(c for c in call("GET", "/api/capture")["open"] if c["name"] == "Mosaic test capture")
        check(st["width"] == 1280 and st["fps"] > 20, f"the device runs ({st['width']}x{st['height']}, {st['fps']:.0f} fps)")
        call("DELETE", "/api/outputs/cap-ndi")
        call("DELETE", "/api/sources/cap")
        # The same bars arriving as UYVY (how NDI sources come in), unpacked to RGB on the GPU.
        call("POST", "/api/sources", {"id": "capu", "name": "Capture UYVY", "type": "capture", "device": "Mosaic test capture (UYVY)", "rect": {"x": 0, "y": 0, "w": 64, "h": 36}})
        call("POST", "/api/outputs", {"id": "capu-ndi", "name": "Capture UYVY (NDI)", "source": "capu", "crop": {"x": 0, "y": 0, "w": 1280, "h": 720}, "width": 1280, "height": 720, "pixelFormat": "bgra"})
        time.sleep(1.5)
        img = frame("capu-ndi")
        want = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
        got = [pixel(img, x, 360) for x in range(80, 1280, 160)]
        check(all(close(g, w_, 6) for g, w_ in zip(got, want)), f"a UYVY source is unpacked to the right colours ({got})")
        call("DELETE", "/api/outputs/capu-ndi")
        call("DELETE", "/api/sources/capu")

        print("display outputs:", flush=True)
        call("PATCH", "/api/outputs/out-5", {"display": "\\\\.\\DISPLAY99"})
        time.sleep(1)
        m5 = next(o for o in json.loads(get("/api/metrics")[1])["outputs"] if o["id"] == "out-5")
        check(m5["sink"] == "display" and m5.get("error"), f"an output on a monitor that isn't there says why ({m5.get('error')})")
        call("PATCH", "/api/outputs/out-5", {"delayMs": 250, "displayMode": {"width": 1920, "height": 1080, "refreshHz": 60}})
        o5 = next(o for o in json.loads(get("/api/layout")[1])["outputs"] if o["id"] == "out-5")
        check(o5.get("delayMs") == 250 and o5.get("displayMode") == {"width": 1920, "height": 1080, "refreshHz": 60},
              f"a display output keeps its delay and monitor mode ({o5.get('delayMs')}, {o5.get('displayMode')})")
        time.sleep(0.5)
        call("PATCH", "/api/outputs/out-5", {"delayMs": 5000, "displayMode": None})
        o5 = next(o for o in json.loads(get("/api/layout")[1])["outputs"] if o["id"] == "out-5")
        check(o5.get("delayMs") == 1000 and "displayMode" not in o5, f"delay capped at 1 s, mode reset by null ({o5.get('delayMs')}, {o5.get('displayMode')})")
        call("PATCH", "/api/outputs/out-5", {"delayMs": 0})
        time.sleep(1)
        call("PATCH", "/api/outputs/out-5", {"display": ""})
        time.sleep(1)
        m5 = next(o for o in json.loads(get("/api/metrics")[1])["outputs"] if o["id"] == "out-5")
        check(m5["sink"] != "display", "back to an NDI stream")
        gpus = call("GET", "/api/gpus")["gpus"]
        print(f"  (GPUs: {[g['name'] for g in gpus]})")

        print("canvases:", flush=True)
        r = call("POST", "/api/canvases", {"name": "Wall Two"})
        c2 = r["id"]
        call("POST", "/api/walls", {"id": "w2", "name": "Wall two", "cols": 2, "rows": 1, "outputWidth": 640, "outputHeight": 360}, canvas=c2)
        l2 = json.loads(get("/api/layout", canvas=c2)[1])
        names2 = [o["ndiName"] for o in l2["outputs"]]
        check(names2 == ["Wall_Two_01", "Wall_Two_02"], f"a new canvas names its streams after itself ({names2})")
        check(len(json.loads(get("/api/layout")[1])["outputs"]) == 5, "the first canvas is untouched")
        time.sleep(1.5)
        check(json.loads(get("/api/metrics", canvas=c2)[1])["engine"]["renderFps"] > 5, "the second canvas renders at the same time")
        call("PATCH", "/api/outputs/out-5", {"display": "\\\\.\\DISPLAY98"})
        time.sleep(0.5)
        call("PATCH", "/api/outputs/w2-1", {"display": "\\\\.\\DISPLAY98"}, canvas=c2)
        time.sleep(1)
        e2 = next(o for o in json.loads(get("/api/metrics", canvas=c2)[1])["outputs"] if o["id"] == "w2-1").get("error", "")
        check("already used by" in e2, f"a monitor another canvas shows is refused ({e2})")
        call("PATCH", "/api/outputs/w2-1", {"display": ""}, canvas=c2)
        call("PATCH", "/api/outputs/out-5", {"display": ""})
        time.sleep(1)
        call("POST", "/api/endpoints", {"name": "Two A", "host": "two-a", "player": "mosaic", "displays": [{"output": "w2-1"}]}, canvas=c2)
        call("POST", "/api/endpoints", {"name": "Two B", "host": "two-b", "player": "mosaic", "displays": [{"output": "w2-2"}]}, canvas=c2)
        r = call("POST", "/api/endpoints/checkin", {"host": "two-a", "screens": [], "panels": []})
        check(r["layout"] == l2["id"] and r["displays"][0]["stream"].endswith("(Wall_Two_01)"), "a PC set up on the second canvas gets its streams")
        shown = lambda off: [[1000 + i, 5_000_000 + i * 33333 + off] for i in range(40)]
        call("POST", "/api/endpoints/checkin", {"host": "two-a", "screens": [], "panels": [], "receiver": {"shownAtEngine": shown(0)}})
        call("POST", "/api/endpoints/checkin", {"host": "two-b", "screens": [], "panels": [], "receiver": {"shownAtEngine": shown(6000)}})
        gs = json.loads(get("/api/endpoints", canvas=c2)[1])["groupSync"]
        check(gs.get("pcs") == 2 and abs(gs.get("spreadMs", 0) - 6) < 0.5, f"the canvas's PCs are compared ({gs})")
        check(any(x["host"] == "two-a" for x in json.loads(get("/api/endpoints")[1]).get("elsewhere", [])), "the first canvas lists them as on another canvas")
        call("POST", f"/api/canvases/{c2}/simulate-gpu-loss", {})
        back = False
        for _ in range(40):
            time.sleep(0.5)
            c = next(x for x in call("GET", "/api/canvases")["canvases"] if x["id"] == c2)
            if c["restarts"] >= 1 and c["running"] and not c["error"] and c.get("renderFps", 0) > 5:
                back = True
                break
        check(back, "a canvas whose GPU device is lost starts again by itself")
        check(json.loads(get("/api/metrics")[1])["engine"]["renderFps"] > 5, "the other canvas kept running")
        call("DELETE", f"/api/canvases/{c2}")
        try:
            call("PATCH", "/api/outputs/w2-1", {"name": "x"}, canvas=c2)
            refused = False
        except urllib.error.HTTPError as ex:
            refused = ex.code == 409
        check(refused, "a change aimed at a stopped canvas is refused")
        check(len(call("GET", "/api/canvases")["canvases"]) == 1, "the canvas is stopped")

        print("frame delivery:", flush=True)
        time.sleep(2)
        m = json.loads(get("/api/metrics")[1])
        json.dump(m, open(os.path.join(args.out, "metrics.json"), "w"), indent=2)
        s = m["sync"]
        check(m["engine"]["frameId"] > 30, f"rendered {m['engine']['frameId']} frames")
        check(all(o["framesSent"] > 0 for o in m["outputs"]), "every output sent frames")
        check(s["framesPartial"] == 0, f"no partial frames ({s['framesComplete']} complete, {s['framesPartial']} partial)")

        for i in range(1, 6):
            try:
                open(os.path.join(args.out, f"out-{i}.jpg"), "wb").write(get(f"/api/outputs/out-{i}/snapshot.jpg")[1])
            except Exception as ex:
                print(f"  (snapshot out-{i} failed: {ex})")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()

print(f"\n{'FAILED: ' + str(len(failures)) + ' check(s)' if failures else 'ALL CHECKS PASSED'}")
sys.exit(1 if failures else 0)
