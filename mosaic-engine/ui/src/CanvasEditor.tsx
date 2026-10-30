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
}

export const COLORS = ["#ff5c5c", "#4cc9f0", "#80ed99", "#ffd166", "#c77dff", "#ff9f1c", "#2ec4b6", "#f72585"];
const SOURCE_COLOR = "#e7e9ee";

const MIN = 16;

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

interface Item {
  id: string;
  label: string;
  rect: Rect;
  angle: number;
  color: string;
  enabled: boolean;
}

// The canvas with draggable rectangles: output crops (which can be tilted), or source placements.
export function CanvasEditor({
  layout,
  mode,
  selectedId,
  snap,
  onSelect,
  onRectChange,
  onAngleChange,
}: {
  layout: Layout;
  mode: EditMode;
  selectedId: string | null;
  snap: number;
  onSelect: (id: string | null) => void;
  onRectChange: (id: string, rect: Rect, final: boolean) => void;
  onAngleChange: (id: string, angle: number, final: boolean) => void;
}) {
  const stageRef = useRef<HTMLDivElement>(null);
  const [box, setBox] = useState({ w: 800, h: 450 });
  const [drag, setDrag] = useState<Drag | null>(null);
  const [live, setLive] = useState<Record<string, Rect>>({});
  const [liveAngle, setLiveAngle] = useState<Record<string, number>>({});

  useEffect(() => {
    const el = stageRef.current;
    if (!el) return;
    const ro = new ResizeObserver(() => setBox({ w: el.clientWidth, h: el.clientHeight }));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);

  const { width: cw, height: ch } = layout.canvas;
  const scale = Math.min(box.w / cw, box.h / ch);

  useEffect(() => {
    if (!drag) return;
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
      const rect = applyDrag(drag, (e.clientX - drag.startX) / scale, (e.clientY - drag.startY) / scale, snap);
      setLive({ [drag.id]: rect });
      onRectChange(drag.id, rect, false);
    };
    const up = (e: PointerEvent) => {
      if (drag.handle === "rotate") onAngleChange(drag.id, angleAt(e), true);
      else onRectChange(drag.id, applyDrag(drag, (e.clientX - drag.startX) / scale, (e.clientY - drag.startY) / scale, snap), true);
      setDrag(null);
      setLive({});
      setLiveAngle({});
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    return () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
  }, [drag, scale, snap, onRectChange, onAngleChange]);

  const begin = (e: React.PointerEvent, it: Item, handle: Handle) => {
    e.stopPropagation();
    e.preventDefault();
    onSelect(it.id);
    const box = (e.currentTarget.closest(".slice") ?? e.currentTarget).getBoundingClientRect();
    setDrag({
      id: it.id,
      handle,
      startX: e.clientX,
      startY: e.clientY,
      start: it.rect,
      angle: it.angle,
      centre: { x: box.left + box.width / 2, y: box.top + box.height / 2 },
    });
  };

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

  return (
    <div className="stage" ref={stageRef} onPointerDown={() => onSelect(null)}>
      <div className="canvas" style={{ width: cw * scale, height: ch * scale }}>
        <LivePreview target="canvas" className="canvas-preview" />
        {items.map((it) => {
          const r = live[it.id] ?? it.rect;
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
      </div>
    </div>
  );
}
