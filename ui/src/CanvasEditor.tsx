import { useEffect, useRef, useState } from "react";
import type { Layout, Rect } from "./api";
import { LivePreview } from "./LivePreview";

type Handle = "move" | "n" | "s" | "e" | "w" | "ne" | "nw" | "se" | "sw";

interface Drag {
  id: string;
  handle: Handle;
  startX: number;
  startY: number;
  start: Rect;
}

export const COLORS = ["#ff5c5c", "#4cc9f0", "#80ed99", "#ffd166", "#c77dff", "#ff9f1c", "#2ec4b6", "#f72585"];

const MIN = 16;

function applyDrag(d: Drag, dx: number, dy: number, snap: number): Rect {
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

export function CanvasEditor({
  layout,
  selectedId,
  snap,
  onSelect,
  onCropChange,
}: {
  layout: Layout;
  selectedId: string | null;
  snap: number;
  onSelect: (id: string | null) => void;
  onCropChange: (id: string, crop: Rect, final: boolean) => void;
}) {
  const stageRef = useRef<HTMLDivElement>(null);
  const [box, setBox] = useState({ w: 800, h: 450 });
  const [drag, setDrag] = useState<Drag | null>(null);
  const [live, setLive] = useState<Record<string, Rect>>({});

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
    const move = (e: PointerEvent) => {
      const crop = applyDrag(drag, (e.clientX - drag.startX) / scale, (e.clientY - drag.startY) / scale, snap);
      setLive({ [drag.id]: crop });
      onCropChange(drag.id, crop, false);
    };
    const up = (e: PointerEvent) => {
      const crop = applyDrag(drag, (e.clientX - drag.startX) / scale, (e.clientY - drag.startY) / scale, snap);
      onCropChange(drag.id, crop, true);
      setDrag(null);
      setLive({});
    };
    window.addEventListener("pointermove", move);
    window.addEventListener("pointerup", up);
    return () => {
      window.removeEventListener("pointermove", move);
      window.removeEventListener("pointerup", up);
    };
  }, [drag, scale, snap, onCropChange]);

  const begin = (e: React.PointerEvent, id: string, handle: Handle, start: Rect) => {
    e.stopPropagation();
    e.preventDefault();
    onSelect(id);
    setDrag({ id, handle, startX: e.clientX, startY: e.clientY, start });
  };

  return (
    <div className="stage" ref={stageRef} onPointerDown={() => onSelect(null)}>
      <div className="canvas" style={{ width: cw * scale, height: ch * scale }}>
        <LivePreview target="canvas" className="canvas-preview" />
        {layout.outputs.map((o, i) => {
          if (o.source !== "canvas") return null;
          const r = live[o.id] ?? o.crop;
          const selected = o.id === selectedId;
          const color = COLORS[i % COLORS.length];
          return (
            <div
              key={o.id}
              className={`slice${selected ? " selected" : ""}${o.enabled ? "" : " disabled"}`}
              style={{ left: r.x * scale, top: r.y * scale, width: r.w * scale, height: r.h * scale, borderColor: color }}
              onPointerDown={(e) => begin(e, o.id, "move", o.crop)}
            >
              <span className="slice-label" style={{ background: color }}>
                {o.name}
                {o.rotation ? ` ↻${o.rotation}°` : ""}
              </span>
              <span className="slice-size">
                {r.x},{r.y} {r.w}×{r.h}
              </span>
              {selected &&
                (["n", "s", "e", "w", "ne", "nw", "se", "sw"] as Handle[]).map((h) => (
                  <span key={h} className={`handle h-${h}`} onPointerDown={(e) => begin(e, o.id, h, o.crop)} />
                ))}
            </div>
          );
        })}
      </div>
    </div>
  );
}
