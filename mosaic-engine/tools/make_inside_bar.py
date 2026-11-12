#!/usr/bin/env python3
"""Generates layouts/inside-bar.json: the inside bar wall, from its measurements.

The wall is 26 ft 11 in wide and 11 ft 10 in from the bottom of the lowest TV to the ceiling.
Centre: a 2 x 2 of 98" 4K panels, sent as one stream that its receiving PC splits. Each side (mirrored): two 40" TVs mounted upright (portrait)
next to the centre, two 40" TVs tilted 45 degrees, and a 24" tilted 45 degrees.

Everything is placed in inches (x from the wall's left edge, y down from the ceiling, positions
read off the photo of the cardboard templates) and turned into canvas pixels at PX_PER_INCH, so
a picture spread over the wall keeps its proportions and the gaps between screens are left out
of it, like bezels. Change the numbers and run again:

    python3 tools/make_inside_bar.py
"""
import json, math, os

WALL_W, WALL_H = 26 * 12 + 11, 11 * 12 + 10  # 323 x 142 in


def tv(diagonal):
    """Width and height in inches of a 16:9 screen."""
    return diagonal * 16 / math.hypot(16, 9), diagonal * 9 / math.hypot(16, 9)


BIG_W, BIG_H = tv(98)   # 85.4 x 48.0
MID_W, MID_H = tv(40)   # 34.9 x 19.6
SMALL_W, SMALL_H = tv(24)  # 20.9 x 11.8

# One centre panel is 1920 canvas pixels wide: a 1080p source on one panel, or a 4K one across
# the 2 x 2, is shown pixel for pixel. (Twice this would be the panels' full 4K, at four times
# the rendering and streaming load.)
PX_PER_INCH = 1920 / BIG_W

CENTRE_X = (WALL_W - 2 * BIG_W) / 2  # centred on the wall
CENTRE_Y = 36.7                      # top of the 2 x 2 below the ceiling

# The left side: (name, centre x, centre y, kind). The right side mirrors it.
LEFT = [
    ("tilted top", 52.0, 37.5, "tilt40"),
    ("tilted outer", 22.7, 71.9, "tilt40"),
    ("small tilted", 31.1, 95.8, "tilt24"),
    ("portrait top", 60.4, 80.8, "portrait40"),
    ("portrait bottom", 60.4, 123.6, "portrait40"),
]
TILT = 45  # degrees; the left side leans counter-clockwise (long edge rising to the right)


def px(inches):
    return int(round(inches * PX_PER_INCH))


def output(oid, name, n, cx, cy, kind, side):
    w, h = (SMALL_W, SMALL_H) if kind == "tilt24" else (MID_W, MID_H)
    o = {"id": oid, "name": name, "ndiName": f"Inside_Bar_{n:02d}", "width": 1280, "height": 720,
         "rotation": 0, "scaleMode": "stretch", "source": "canvas", "enabled": True}
    if kind == "portrait40":
        w, h = h, w
        o["rotation"] = 90  # mounted on its side; 270 if the picture comes out upside down
    else:
        o["angle"] = -TILT if side == "left" else TILT  # 180 more if the picture comes out upside down
    o["crop"] = {"x": px(cx - w / 2), "y": px(cy - h / 2), "w": px(w), "h": px(h)}
    return o


def main():
    canvas_w, canvas_h = px(WALL_W) // 2 * 2, px(WALL_H) // 2 * 2
    gx, gy = px(CENTRE_X), px(CENTRE_Y)
    outputs, n = [], 0
    # The centre 2 x 2 goes out as ONE 3840 x 2160 stream: its receiving PC cuts it across the four
    # panels (Endpoints -> "+ 2x2 from one output"), so they always show pieces of the same frame
    # and the media server encodes one stream instead of four.
    n += 1
    outputs.append({"id": "centre-1", "name": "Centre 2x2", "ndiName": f"Inside_Bar_{n:02d}", "width": 3840, "height": 2160,
                    "crop": {"x": gx, "y": gy, "w": 3840, "h": 2160},
                    "rotation": 0, "scaleMode": "stretch", "source": "canvas", "group": "centre", "enabled": True})
    for side in ("left", "right"):
        for name, cx, cy, kind in LEFT:
            n += 1
            x = cx if side == "left" else WALL_W - cx
            outputs.append(output(f"{side}-{name.replace(' ', '-')}", f"{side.capitalize()} {name}", n, x, cy, kind, side))
    layout = {
        "id": "inside-bar", "name": "Inside bar - 2x2 of 98\" with 40\" and 24\" side screens", "frameRate": 30, "ndiPrefix": "Inside_Bar",
        "canvas": {"width": canvas_w, "height": canvas_h, "background": [0, 0, 0, 1]},
        "sources": [{"id": "pattern", "name": "Test pattern", "type": "test_pattern", "rect": {"x": 0, "y": 0, "w": canvas_w, "h": canvas_h}, "enabled": True}],
        "walls": [{"id": "centre", "name": "Centre 2x2", "cols": 1, "rows": 1, "outputWidth": 3840, "outputHeight": 2160, "rotation": 0,
                   "gapX": 0, "gapY": 0, "x": gx, "y": gy, "pixelFormat": "uyvy", "fitCanvas": False}],
        "outputs": outputs,
    }
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "layouts", "inside-bar.json")
    with open(path, "w") as f:
        json.dump(layout, f, indent=2)
        f.write("\n")
    print(f"{os.path.normpath(path)}: canvas {canvas_w}x{canvas_h} ({PX_PER_INCH:.2f} px/in), {len(outputs)} outputs")


if __name__ == "__main__":
    main()
