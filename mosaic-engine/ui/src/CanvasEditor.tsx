import { useEffect, useRef, useState } from "react";
import type { Layout, Rect } from "./api";
import { LivePreview } from "./LivePreview";

type Handle = "move" | "rotate" | "n" | "s" | "e" | "w" | "ne" | "nw" | "se" | "sw";
export type EditMode = "outputs" | "sources";

interface Drag {
  id: string;
  handle: Handle;
  startX: number;
  startY: number;
  start: Rect;
  angle: number; // the item's tilt when the drag began
  centre: { x: number; y: number }; // its centre in client pixels (for rotating)
  group?: string; // dragging a whole wall group (start = its bounds)
}

interface Guides {
  x: number[];
  y: number[];
}

export const COLORS = ["#ff5c5c", "#4cc9f0", "#80ed99", "#ffd166", "#c77dff", "#ff9f1c", "#2ec4b6", "#f72585"];
const SOURCE_COLOR = "#e7e9ee";

const MIN = 16;
const SNAP_PX = 8; // smart-snap distance, in screen pixels

// Resizing a tilted rectangle: work in its own (rotated) frame, then move its
// centre so the opposite edge stays put on the canvas.
function applyTiltedResize(d: Drag, dx: number, dy: number): Rect {
  const a = (d.angle * Math.PI) / 180;
  const cos = Math.cos(a);
  const sin = Math.sin(a);
  const lx = dx * cos + dy * sin; // canvas delta in the rectangle's frame
  const ly = -dx * sin + dy * cos;
  let { w, h } = d.start;
  let cx = 0;
  let cy = 0;
  if (d.handle.includes("e")) (w = Math.max(MIN, w + lx), (cx = (w - d.start.w) / 2));
  if (d.handle.includes("w")) (w = Math.max(MIN, w - lx), (cx = -(w - d.start.w) / 2));
  if (d.handle.includes("s")) (h = Math.max(MIN, h + ly), (cy = (h - d.start.h) / 2));
  if (d.handle.includes("n")) (h = Math.max(MIN, h - ly), (cy = -(h - d.start.h) / 2));
  const centreX = d.start.x + d.start.w / 2 + cx * cos - cy * sin;
  const centreY = d.start.y + d.start.h / 2 + cx * sin + cy * cos;
  return { x: Math.round(centreX - w / 2), y: Math.round(centreY - h / 2), w: Math.round(w), h: Math.round(h) };
}

function applyDrag(d: Drag, dx: number, dy: number, snap: number): Rect {
  if (d.angle !== 0 && d.handle !== "move") return applyTiltedResize(d, dx, dy);
  const s = (v: number) => (snap > 1 ? Math.round(v / snap) * snap : Math.round(v));
  let { x, y, w, h } = d.start;
  const right = x + w;
  const bottom = y + h;
  if (d.handle === "move") return { x: s(x + dx), y: s(y + dy), w, h };
  if (d.handle.includes("w")) x = Math.min(s(x + dx), right - MIN);
  if (d.handle.includes("e")) w = Math.max(MIN, s(right + dx) - x);
  else w = right - x;
  if (d.handle.includes("n")) y = Math.min(s(y + dy), bottom - MIN);
  if (d.handle.includes("s")) h = Math.max(MIN, s(bottom + dy) - y);
  else h = bottom - y;
  return { x, y, w, h };
}

// Pulls the moving edges (and, when moving, the centre) onto the nearest edge or centre
// of another frame or of the canvas, if one is within `dist`. Returns the lines it used.
function smartSnap(r: Rect, handle: Handle, targets: Rect[], canvas: Rect, dist: number): { rect: Rect; guides: Guides } {
  const xs: number[] = [];
  const ys: number[] = [];
  for (const t of [canvas, ...targets]) {
    xs.push(t.x, t.x + t.w / 2, t.x + t.w);
    ys.push(t.y, t.y + t.h / 2, t.y + t.h);
  }
  const nearest = (lines: number[], cands: number[]) => {
    let best: { delta: number; line: number } | null = null;
    for (const c of cands)
      for (const l of lines) {
        const delta = l - c;
        if (Math.abs(delta) <= dist && (!best || Math.abs(delta) < Math.abs(best.delta))) best = { delta, line: l };
      }
    return best;
  };
  const out = { ...r };
  const guides: Guides = { x: [], y: [] };
  if (handle === "move") {
    const bx = nearest(xs, [r.x, r.x + r.w / 2, r.x + r.w]);
    if (bx) (out.x = Math.round(r.x + bx.delta), guides.x.push(bx.line));
    const by = nearest(ys, [r.y, r.y + r.h / 2, r.y + r.h]);
    if (by) (out.y = Math.round(r.y + by.delta), guides.y.push(by.line));
    return { rect: out, guides };
  }
  if (handle.includes("w")) {
    const b = nearest(xs, [r.x]);
    if (b && r.w - b.delta >= MIN) (out.x = Math.round(r.x + b.delta), (out.w = Math.round(r.w - b.delta)), guides.x.push(b.line));
  } else if (handle.includes("e")) {
    const b = nearest(xs, [r.x + r.w]);
    if (b && r.w + b.delta >= MIN) (out.w = Math.round(r.w + b.delta), guides.x.push(b.line));
  }
  if (handle.includes("n")) {
    const b = nearest(ys, [r.y]);
    if (b && r.h - b.delta >= MIN) (out.y = Math.round(r.y + b.delta), (out.h = Math.round(r.h - b.delta)), guides.y.push(b.line));
  } else if (handle.includes("s")) {
    const b = nearest(ys, [r.y + r.h]);
    if (b && r.h + b.delta >= MIN) (out.h = Math.round(r.h + b.delta), guides.y.push(b.line));
  }
  return { rect: out, guides };
}

function bounds(rects: Rect[]): Rect {
  const x = Math.min(...rects.map((r) => r.x));
  const y = Math.min(...rects.map((r) => r.y));
  const x2 = Math.max(...rects.map((r) => r.x + r.w));
  const y2 = Math.max(...rects.map((r) => r.y + r.h));
  return { x, y, w: x2 - x, h: y2 - y };
}

interface Item {
  id: string;
  label: string;
  rect: Rect;
  angle: number;
  color: string;
  enabled: boolean;
  group?: string;
}

// The canvas with draggable rectangles: output crops (which can be tilted), or source placements.
// In outputs mode each wall group gets a dashed frame whose tab drags the whole group.
export function CanvasEditor({
  layout,
  mode,
  selectedId,
  snap,
  smart,
  onSelect,
  onRectChange,
  onAngleChange,
  onGroupMove,
}: {
  layout: Layout;
  mode: EditMode;
  selectedId: string | null;
  snap: number;
  smart: boolean;
  onSelect: (id: string | null) => void;
  onRectChange: (id: string, rect: Rect, final: boolean) => void;
  onAngleChange: (id: string, angle: number, final: boolean) => void;
  onGroupMove: (id: string, x: number, y: number) => void;
}) {
  const stageRef = useRef<HTMLDivElement>(null);
  const [box, setBox] = useState({ w: 800, h: 450 });
  const [drag, setDrag] = useState<Drag | null>(null);
  const [live, setLive] = useState<Record<string, Rect>>({});
  const [liveAngle, setLiveAngle] = useState<Record<string, number>>({});
  const [liveGroup, setLiveGroup] = useState<{ id: string; dx: number; dy: number } | null>(null);
  const [guides, setGuides] = useState<Guides>({ x: [], y: [] });

  useEffect(() => {
    const el = stageRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setBox({ w: el.clientWidth, h: el.clientHeight }));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const { width: cw, height: ch } = layout.canvas;
  const scale = Math.min(box.w / cw, box.h / ch);

  const items: Item[] =
    mode === "outputs"
      ? layout.outputs.flatMap((o, i) =>
          o.source === "canvas"
            ? [
                {
                  id: o.id,
                  label: o.name + (o.rotation ? ` ↻${o.rotation}°` : "") + (o.angle ? ` ∠${+o.angle.toFixed(1)}°` : ""),
                  rect: o.crop,
                  angle: o.angle ?? 0,
                  color: COLORS[i % COLORS.length],
                  enabled: o.enabled,
                  group: o.group && layout.walls.some((w) => w.id === o.group) ? o.group : undefined,
                },
              ]
            : [],
        )
      : layout.sources.map((s) => ({
          id: s.id,
          label: s.name + (s.opacity < 1 ? ` · ${Math.round(s.opacity * 100)}%` : ""),
          rect: s.rect,
          angle: 0,
          color: SOURCE_COLOR,
          enabled: s.enabled,
        }));

  const groups = layout.walls.flatMap((w) => {
    const rects = items.filter((it) => it.group === w.id).map((it) => it.rect);
    return rects.length ? [{ id: w.id, name: w.name, rect: bounds(rects), count: rects.length }] : [];
  });

  useEffect(() => {
    if (!drag) return;
    // Everything else on the canvas (outputs and sources) is something to line up with.
    const others = [
      ...layout.outputs.filter((o) => o.source === "canvas" && o.id !== drag.id && (!drag.group || o.group !== drag.group)).map((o) => o.crop),
      ...layout.sources.filter((s) => s.id !== drag.id).map((s) => s.rect),
    ];
    const canvasRect = { x: 0, y: 0, w: cw, h: ch };
    const place = (e: PointerEvent): Rect => {
      const r = applyDrag(drag, (e.clientX - drag.startX) / scale, (e.clientY - drag.startY) / scale, snap);
      if (!smart || e.altKey || (drag.angle !== 0 && drag.handle !== "move")) {
        setGuides({ x: [], y: [] });
        return r;
      }
      const res = smartSnap(r, drag.handle, others, canvasRect, SNAP_PX / scale);
      setGuides(res.guides);
      return res.rect;
    };
    // Rotate handle: angle from the centre to the pointer (Shift snaps to 15°, else 0.5°).
    const angleAt = (e: PointerEvent) => {
      const raw = (Math.atan2(e.clientY - drag.centre.y, e.clientX - drag.centre.x) * 180) / Math.PI + 90;
      const step = e.shiftKey ? 15 : 0.5;
      const a = Math.round(raw / step) * step;
      return ((((a + 180) % 360) + 360) % 360) - 180;
    };
    const move = (e: PointerEvent) => {
      if (drag.handle === "rotate") {
        const a = angleAt(e);
        setLiveAngle({ [drag.id]: a });
        onAngleChange(drag.id, a, false);
        return;
      }
      const rect = place(e);
      if (drag.group) {
        setLiveGroup({ id: drag.group, dx: rect.x - drag.start.x, dy: rect.y - drag.start.y });
        return;
      }
      setLive({ [drag.id]: rect });
      onRectChange(drag.id, rect, false);
    };
    const up = (e: PointerEvent) => {
      if (drag.handle === "rotate") onAngleChange(drag.id, angleAt(e), true);
      else if (drag.group) {
        const rect = place(e);
        const wall = layout.walls.find((w) => w.id === drag.group);
        // The wall's x/y is its first panel's corner; shift it by however far the frame moved.
        if (wall && (rect.x !== drag.start.x || rect.y !== drag.start.y))
          onGroupMove(drag.group, wall.x + rect.x - drag.start.x, wall.y + rect.y - drag.start.y);
        else setLiveGroup(null);
      } else onRectChange(drag.id, place(e), true);
      setDrag(null);
      setLive({});
      setLiveAngle({});
      setGuides({ x: [], y: [] });
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    return () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
  }, [drag, scale, snap, smart, cw, ch, layout, onRectChange, onAngleChange, onGroupMove]);

  // The group's new position arrives with the next layout; drop the preview offset then.
  useEffect(() => setLiveGroup(null), [layout]);

  const begin = (e: React.PointerEvent, it: Item, handle: Handle) => {
    e.stopPropagation();
    e.preventDefault();
    onSelect(it.id);
    const box = (e.currentTarget.closest(".slice") ?? e.currentTarget).getBoundingClientRect();
    // A single-display group moves as a group, so the group remembers where it is.
    const solo = handle === "move" && it.group ? groups.find((g) => g.id === it.group && g.count === 1) : undefined;
    setDrag({
      id: it.id,
      handle,
      startX: e.clientX,
      startY: e.clientY,
      start: solo ? solo.rect : it.rect,
      angle: it.angle,
      centre: { x: box.left + box.width / 2, y: box.top + box.height / 2 },
      group: solo?.id,
    });
  };

  const beginGroup = (e: React.PointerEvent, g: (typeof groups)[number]) => {
    e.stopPropagation();
    e.preventDefault();
    setDrag({ id: g.id, handle: "move", startX: e.clientX, startY: e.clientY, start: g.rect, angle: 0, centre: { x: 0, y: 0 }, group: g.id });
  };

  const shift = (r: Rect, group?: string) => (liveGroup && group === liveGroup.id ? { ...r, x: r.x + liveGroup.dx, y: r.y + liveGroup.dy } : r);

  return (
    <div className="stage" ref={stageRef} onPointerDown={() => onSelect(null)}>
      <div className="canvas" style={{ width: cw * scale, height: ch * scale }}>
        <LivePreview target="canvas" className="canvas-preview" />
        {items.map((it) => {
          const r = shift(live[it.id] ?? it.rect, it.group);
          const angle = liveAngle[it.id] ?? it.angle;
          const selected = it.id === selectedId;
          return (
            <div
              key={it.id}
              className={`slice${selected ? " selected" : ""}${it.enabled ? "" : " disabled"}${mode === "sources" ? " src" : ""}`}
              style={{
                left: r.x * scale,
                top: r.y * scale,
                width: r.w * scale,
                height: r.h * scale,
                borderColor: it.color,
                transform: angle ? `rotate(${angle}deg)` : undefined,
              }}
              onPointerDown={(e) => begin(e, it, "move")}
            >
              <span className="slice-label" style={{ background: it.color }}>
                {it.label}
              </span>
              <span className="slice-size">
                {r.x},{r.y} {r.w}×{r.h}
              </span>
              {selected &&
                (["n", "s", "e", "w", "ne", "nw", "se", "sw"] as Handle[]).map((h) => (
                  <span key={h} className={`handle h-${h}`} onPointerDown={(e) => begin(e, it, h)} />
                ))}
              {selected && mode === "outputs" && (
                <span className="rotate-handle" title="Drag to tilt (Shift: 15° steps)" onPointerDown={(e) => begin(e, it, "rotate")} />
              )}
            </div>
          );
        })}
        {mode === "outputs" &&
          groups
            .filter((g) => g.count > 1)
            .map((g) => {
              const r = shift(g.rect, g.id);
              return (
                <div key={g.id} className="group-frame" style={{ left: r.x * scale, top: r.y * scale, width: r.w * scale, height: r.h * scale }}>
                  <span className="group-tab" title="Drag to move the whole group" onPointerDown={(e) => beginGroup(e, g)}>
                    ⠿ {g.name}
                  </span>
                </div>
              );
            })}
        {guides.x.map((x, i) => (
          <div key={`gx${i}`} className="guide v" style={{ left: x * scale }} />
        ))}
        {guides.y.map((y, i) => (
          <div key={`gy${i}`} className="guide h" style={{ top: y * scale }} />
        ))}
      </div>
    </div>
  );
}
