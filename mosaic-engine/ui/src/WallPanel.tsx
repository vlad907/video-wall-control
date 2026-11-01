import { useState } from "react";
import type { Layout, Wall } from "./api";
import { NumberField } from "./Inspector";

const PANELS: [string, number, number][] = [
  ["1920 × 1080", 1920, 1080],
  ["3840 × 2160", 3840, 2160],
  ["3840 × 1080 (double-wide)", 3840, 1080],
  ["5120 × 1440 (double-wide)", 5120, 1440],
  ["1280 × 720", 1280, 720],
];

// Starting points for a new group: [label, name, cols, rows, panel width, panel height].
const PRESETS: [string, string, number, number, number, number][] = [
  ["Single 1080p", "Display", 1, 1, 1920, 1080],
  ["Single 4K", "4K display", 1, 1, 3840, 2160],
  ["Double-wide", "Double wide", 1, 1, 3840, 1080],
  ["2 × 2", "2x2 wall", 2, 2, 1920, 1080],
  ["4 × 4", "Main wall", 4, 4, 1920, 1080],
  ["3 × 3 of 4K", "4K wall", 3, 3, 3840, 2160],
];

const DEFAULT: Wall = {
  id: "",
  name: "Wall",
  cols: 2,
  rows: 2,
  outputWidth: 1920,
  outputHeight: 1080,
  rotation: 0,
  gapX: 0,
  gapY: 0,
  x: 0,
  y: 0,
  pixelFormat: "uyvy",
  fitCanvas: true,
};

// Where a new group goes: under everything already on the canvas, or to the right if that runs out of room.
function freeSpot(layout: Layout): { x: number; y: number } {
  const crops = layout.outputs.filter((o) => o.source === "canvas").map((o) => o.crop);
  if (!crops.length) return { x: 0, y: 0 };
  const bottom = Math.max(...crops.map((c) => c.y + c.h));
  if (bottom + 2160 <= 16384) return { x: 0, y: bottom };
  return { x: Math.max(...crops.map((c) => c.x + c.w)), y: 0 };
}

// Edits one wall group: one output per panel of a cols x rows grid, leaving canvas
// pixels out for the bezels so lines continue straight across panel edges.
// A 1 x 1 group is a single display (a double-wide screen is one 3840 x 1080 panel).
export function WallPanel({
  layout,
  wall,
  onApply,
  onRemove,
  onClose,
}: {
  layout: Layout;
  wall: Wall | null; // null = a new group
  onApply: (w: Wall) => void;
  onRemove: (removeOutputs: boolean) => void;
  onClose: () => void;
}) {
  const [w, setW] = useState<Wall>(() => wall ?? { ...DEFAULT, ...freeSpot(layout), fitCanvas: true });
  const [unit, setUnit] = useState<"px" | "mm">("px");
  // mm mode: physical size of the picture area (long side) and of the gaps between pictures.
  const [imageMm, setImageMm] = useState(1210);
  const [gapMm, setGapMm] = useState({ x: 3.5, y: 3.5 });

  const set = (p: Partial<Wall>) => setW((cur) => ({ ...cur, ...p }));
  const pxPerMm = Math.max(w.outputWidth, w.outputHeight) / Math.max(1, imageMm);
  const gapX = unit === "mm" ? Math.round(gapMm.x * pxPerMm) : w.gapX;
  const gapY = unit === "mm" ? Math.round(gapMm.y * pxPerMm) : w.gapY;
  const sideways = w.rotation === 90 || w.rotation === 270;
  const cellW = sideways ? w.outputHeight : w.outputWidth;
  const cellH = sideways ? w.outputWidth : w.outputHeight;
  const totalW = w.x + w.cols * cellW + (w.cols - 1) * gapX;
  const totalH = w.y + w.rows * cellH + (w.rows - 1) * gapY;
  const count = w.cols * w.rows;
  // Output ids, as the engine makes them: out-N for the group "wall", else <group id>-N (the id comes from the name).
  const slug = w.name.toLowerCase().replace(/[ .]/g, "-").replace(/[^a-z0-9_-]/g, "").slice(0, 64) || "group";
  const prefix = wall ? (wall.id === "wall" ? "out" : wall.id) : slug;

  const groupW = totalW - w.x;
  const groupH = totalH - w.y;
  const scale = 260 / Math.max(groupW, groupH * 1.6);

  return (
    <div className="inspector">
      <div className="inspector-head">
        <h2>{wall ? wall.name : "New group"}</h2>
        <button onClick={onClose}>Close</button>
      </div>
      <small className="muted">
        One output per display ({count === 1 ? `${prefix}-1` : `${prefix}-1 … ${prefix}-${count}`}, row by row). Updating keeps their NDI streams.
      </small>

      {!wall && (
        <div className="group">
          <h3>Start from</h3>
          <div className="presets wide-note">
            {PRESETS.map(([label, name, cols, rows, outputWidth, outputHeight]) => (
              <button
                key={label}
                className={w.cols === cols && w.rows === rows && w.outputWidth === outputWidth && w.outputHeight === outputHeight ? "on" : ""}
                onClick={() => set({ name, cols, rows, outputWidth, outputHeight })}
              >
                {label}
              </button>
            ))}
          </div>
        </div>
      )}

      <div className="group">
        <label className="field wide">
          <span>Name</span>
          <input type="text" value={w.name} onChange={(e) => set({ name: e.target.value })} />
        </label>
      </div>

      <div className="group">
        <h3>Grid</h3>
        <NumberField label="Columns" value={w.cols} min={1} onCommit={(cols) => set({ cols: Math.min(16, cols) })} />
        <NumberField label="Rows" value={w.rows} min={1} onCommit={(rows) => set({ rows: Math.min(16, rows) })} />
      </div>

      <div className="group">
        <h3>Panel</h3>
        <select
          value={`${w.outputWidth}x${w.outputHeight}`}
          onChange={(e) => {
            const p = PANELS.find(([, pw, ph]) => `${pw}x${ph}` === e.target.value);
            if (p) set({ outputWidth: p[1], outputHeight: p[2] });
          }}
        >
          {PANELS.map(([label, pw, ph]) => (
            <option key={label} value={`${pw}x${ph}`}>
              {label}
            </option>
          ))}
          {!PANELS.some(([, pw, ph]) => pw === w.outputWidth && ph === w.outputHeight) && (
            <option value={`${w.outputWidth}x${w.outputHeight}`}>
              {w.outputWidth} × {w.outputHeight}
            </option>
          )}
        </select>
        <div className="seg">
          {([0, 90, 180, 270] as const).map((r) => (
            <button key={r} className={w.rotation === r ? "on" : ""} onClick={() => set({ rotation: r })} title={r === 90 || r === 270 ? "portrait" : "landscape"}>
              {r === 0 ? "Landscape" : r === 90 ? "Portrait ↻" : r === 180 ? "180°" : "Portrait ↺"}
            </button>
          ))}
        </div>
      </div>

      <div className="group">
        <h3>Bezel gap</h3>
        <div className="seg">
          {(["px", "mm"] as const).map((u) => (
            <button key={u} className={unit === u ? "on" : ""} onClick={() => setUnit(u)}>
              {u === "px" ? "Pixels" : "Millimetres"}
            </button>
          ))}
        </div>
        {unit === "px" ? (
          <div className="row">
            <NumberField label="Horizontal gap (px)" value={w.gapX} onCommit={(gapX) => set({ gapX })} />
            <NumberField label="Vertical gap (px)" value={w.gapY} onCommit={(gapY) => set({ gapY })} />
          </div>
        ) : (
          <>
            <label className="field wide">
              <span>Picture width, long side (mm)</span>
              <input type="number" value={imageMm} onChange={(e) => setImageMm(+e.target.value)} />
            </label>
            <label className="field">
              <span>Gap between pictures, side by side (mm)</span>
              <input type="number" step="0.1" value={gapMm.x} onChange={(e) => setGapMm({ ...gapMm, x: +e.target.value })} />
            </label>
            <label className="field">
              <span>Gap, above / below (mm)</span>
              <input type="number" step="0.1" value={gapMm.y} onChange={(e) => setGapMm({ ...gapMm, y: +e.target.value })} />
            </label>
            <small className="wide-note muted">
              = {gapX} px × {gapY} px. Measure from the edge of one picture to the edge of the next (both bezels).
            </small>
          </>
        )}
      </div>

      <div className="group">
        <h3>Placement</h3>
        <NumberField label="Left (px)" value={w.x} onCommit={(x) => set({ x })} />
        <NumberField label="Top (px)" value={w.y} onCommit={(y) => set({ y })} />
        <label className="toggle wide-note">
          <input type="checkbox" checked={w.fitCanvas} onChange={(e) => set({ fitCanvas: e.target.checked })} />
          Resize the canvas to fit all groups
        </label>
        <div className="seg">
          {(["uyvy", "bgra"] as const).map((f) => (
            <button key={f} className={w.pixelFormat === f ? "on" : ""} onClick={() => set({ pixelFormat: f })}>
              {f === "uyvy" ? "UYVY 4:2:2" : "BGRA"}
            </button>
          ))}
        </div>
      </div>

      <svg className="wall-diagram" viewBox={`0 0 ${Math.max(1, groupW * scale)} ${Math.max(1, groupH * scale)}`}>
        {Array.from({ length: count }, (_, i) => {
          const c = i % w.cols;
          const r = Math.floor(i / w.cols);
          return (
            <g key={i}>
              <rect
                x={c * (cellW + gapX) * scale}
                y={r * (cellH + gapY) * scale}
                width={cellW * scale}
                height={cellH * scale}
              />
              <text x={(c * (cellW + gapX) + cellW / 2) * scale} y={(r * (cellH + gapY) + cellH / 2) * scale}>
                {i + 1}
              </text>
            </g>
          );
        })}
      </svg>
      <small className="muted">
        {groupW} × {groupH} px on the canvas. Other groups and outputs stay as they are.
      </small>

      <button className="primary" onClick={() => onApply({ ...w, gapX, gapY })}>
        {wall ? "Update group" : "Create group"} ({count} output{count === 1 ? "" : "s"})
      </button>
      {wall && (
        <div className="row buttons">
          <button onClick={() => onRemove(false)} title="Keeps the outputs as standalone outputs, forgets the group">
            Ungroup
          </button>
          <button className="danger" onClick={() => confirm(`Delete "${wall.name}" and its ${count} output(s)?`) && onRemove(true)}>
            Delete group
          </button>
        </div>
      )}
    </div>
  );
}
