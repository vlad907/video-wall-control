#!/usr/bin/env python3
"""Milestone 2 benchmark: runs each layout in BGRA and UYVY and prints a table.

    python tools/bench.py [--engine PATH] [--seconds 30] [--layouts quad-1080p,wall-2x2-4k,wall-6x4-1080p] [--out bench.json]

Uses the engine's headless mode (--run-seconds). Results include render fps,
tick lateness, CPU/GPU time, readback bandwidth, per-output drops and whether
every frame reached every output. With NDI built in, outputs are real NDI
streams; connect receivers before starting to measure under load.
"""
import argparse, json, os, platform, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
default_engine = os.path.join(root, "build", "Release", "mosaic_engine.exe") if os.name == "nt" else os.path.join(root, "build", "mosaic_engine")

ap = argparse.ArgumentParser()
ap.add_argument("--engine", default=default_engine)
ap.add_argument("--seconds", type=float, default=30)
ap.add_argument("--layouts", default="quad-1080p,wall-2x2-4k")
ap.add_argument("--formats", default="bgra,uyvy")
ap.add_argument("--out", default="bench.json")
args = ap.parse_args()

results = []
with tempfile.TemporaryDirectory() as tmp:
    for name in args.layouts.split(","):
        base = json.load(open(os.path.join(root, "layouts", f"{name}.json")))
        for fmt in args.formats.split(","):
            lid = f"{name}-{fmt}"
            ldir = os.path.join(tmp, lid)
            os.makedirs(os.path.join(ldir, "layouts"))
            layout = json.loads(json.dumps(base))
            layout["id"] = lid
            for o in layout["outputs"]:
                o["pixelFormat"] = fmt
            json.dump(layout, open(os.path.join(ldir, "layouts", f"{lid}.json"), "w"))
            print(f"running {lid} for {args.seconds:.0f} s ...", file=sys.stderr, flush=True)
            out = subprocess.run([args.engine, "--data", os.path.join(ldir, "data"), "--layouts", os.path.join(ldir, "layouts"),
                                  "--layout", lid, "--port", "8099", "--run-seconds", str(args.seconds)],
                                 capture_output=True, text=True)
            if out.returncode != 0:
                print(out.stderr, file=sys.stderr)
                sys.exit(f"engine failed on {lid}")
            m = json.loads(out.stdout)
            m["run"] = {"layout": name, "format": fmt, "seconds": args.seconds}
            results.append(m)

hdr = f"{'layout':<16}{'fmt':<6}{'fps':>7}{'late':>6}{'tick ms':>9}{'CPU ms':>8}{'GPU ms':>8}{'util%':>7}{'MB/s':>8}{'lat ms':>8}{'send ms':>9}{'drops':>7}{'complete':>12}"
print(hdr)
print("-" * len(hdr))
for m in results:
    e, o, s, r = m["engine"], m["outputs"], m["sync"], m["run"]
    n = max(1, len(o))
    drops = sum(x["droppedNoBuffer"] + x["droppedQueue"] for x in o)
    total = s["framesComplete"] + s["framesPartial"]
    print(f"{r['layout']:<16}{r['format']:<6}{e['renderFps']:>7.2f}{e['lateFrames']:>6}{e['tickJitterMs']:>9.3f}{e['encodeMs']:>8.2f}"
          f"{e['gpuMs']:>8.2f}{e['gpuUtilization']:>7.0f}{e['readbackMBps']:>8.0f}{sum(x['latencyMs'] for x in o) / n:>8.1f}"
          f"{max(x['sendMsMax'] for x in o):>9.2f}{drops:>7}{s['framesComplete']:>6}/{total:<5}")
print(f"\n{results[0]['engine']['backend']} on {results[0]['engine']['device']}, NDI: {results[0]['engine']['ndi']}, host: {platform.platform()}")
json.dump(results, open(args.out, "w"), indent=2)
print(f"full metrics written to {args.out}")
