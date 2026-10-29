#!/usr/bin/env python3
"""End-to-end check of a built engine, used by CI (Windows/WARP) and locally.

Runs the engine on a frozen test pattern and checks real pixels:
  - every output's corner markers have the right colours in the right corners
  - a 90-degree output has its corners rotated clockwise
  - PATCHing an output's crop over the REST API changes what it renders
  - every rendered frame reached every output (no partial frames)

    python tools/ci_check.py [--engine PATH] [--out DIR]

Set MOSAIC_SOFTWARE_GPU=1 to run on WARP (no GPU needed). Exits non-zero on failure.
"""
import argparse, json, os, subprocess, sys, tempfile, time, urllib.request

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

def get(path, timeout=10):
    with urllib.request.urlopen(BASE + path, timeout=timeout) as r:
        return r.headers, r.read()

def frame(oid):
    for _ in range(30):  # WARP can be slow to produce the first frames
        try:
            h, body = get(f"/api/outputs/{oid}/frame.bgra")
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
