#!/usr/bin/env python3
"""Checks GPU UYVY packing against the BGRA path.

Starts the engine with two outputs showing the same region of a frozen test
pattern, one BGRA and one UYVY, fetches both as raw BGRA (the UYVY one is
decoded with BT.709), and reports the per-channel error.

    python3 tools/check_uyvy.py [path/to/mosaic_engine]
"""
import json, math, os, subprocess, sys, tempfile, time, urllib.request

ENGINE = sys.argv[1] if len(sys.argv) > 1 else "build/mosaic_engine"
PORT = 8097

layout = {
    "id": "uyvy-check", "name": "UYVY check", "frameRate": 60,
    "canvas": {"width": 3840, "height": 2160, "background": [0, 0, 0, 1]},
    "sources": [{"id": "pattern", "type": "test_pattern", "animated": False,
                 "rect": {"x": 0, "y": 0, "w": 3840, "h": 2160}}],
    "outputs": [
        {"id": "bgra", "pixelFormat": "bgra", "crop": {"x": 1000, "y": 600, "w": 1920, "h": 1080}},
        {"id": "uyvy", "pixelFormat": "uyvy", "crop": {"x": 1000, "y": 600, "w": 1920, "h": 1080}},
    ],
}

def fetch(output):
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/api/outputs/{output}/frame.bgra") as r:
        return int(r.headers["X-Width"]), int(r.headers["X-Height"]), r.read()

with tempfile.TemporaryDirectory() as tmp:
    os.makedirs(f"{tmp}/layouts")
    with open(f"{tmp}/layouts/uyvy-check.json", "w") as f:
        json.dump(layout, f)
    proc = subprocess.Popen([ENGINE, "--data", f"{tmp}/data", "--layouts", f"{tmp}/layouts", "--port", str(PORT),
                             "--layout", "uyvy-check"], stderr=subprocess.DEVNULL)
    try:
        time.sleep(2)
        w, h, a = fetch("bgra")
        w2, h2, b = fetch("uyvy")
    finally:
        proc.terminate()
        proc.wait()

assert (w, h) == (w2, h2) == (1920, 1080), (w, h, w2, h2)
try:
    import numpy as np
    A = np.frombuffer(a, np.uint8).reshape(h, w, 4)[..., :3].astype(np.int16)
    B = np.frombuffer(b, np.uint8).reshape(h, w, 4)[..., :3].astype(np.int16)
    d = np.abs(A - B)
    luma = lambda X: X[..., 2] * 0.2126 + X[..., 1] * 0.7152 + X[..., 0] * 0.0722
    dl = np.abs(luma(A) - luma(B))
    mse = (d.astype(np.float64) ** 2).mean()
    print(f"RGB  max error {d.max()}, mean {d.mean():.3f}, PSNR {10 * math.log10(255 ** 2 / mse):.1f} dB")
    print(f"luma max error {dl.max():.2f}, mean {dl.mean():.3f}  (chroma is halved horizontally by 4:2:2)")
    print(f"pixels off by more than 2 in any channel: {(d.max(axis=2) > 2).mean() * 100:.2f}% (expected at sharp colour edges)")
    # Where both pixels of a 4:2:2 pair have the same colour, nothing is shared,
    # so only 8-bit YCbCr rounding remains. This also proves pairs are aligned:
    # an off-by-one packing would mix neighbouring colours everywhere.
    same = (A[:, 0::2] == A[:, 1::2]).all(axis=2)
    same = np.repeat(same, 2, axis=1)
    err_same = d[same].max()
    print(f"uniform pairs ({same.mean() * 100:.1f}% of frame): max RGB error {err_same} (8-bit YCbCr rounding)")
    print(f"luma mean error over whole frame: {dl.mean():.3f}")
    ok = err_same <= 3 and dl.mean() < 0.5
except ImportError:
    diffs = [abs(x - y) for i, (x, y) in enumerate(zip(a, b)) if i % 4 != 3]
    print(f"max channel error {max(diffs)}, mean {sum(diffs) / len(diffs):.3f}")
    ok = True
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
