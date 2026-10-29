import { useEffect, useState } from "react";
import type { Layout, Output, Rect } from "./api";
import { LivePreview } from "./LivePreview";

const RESOLUTIONS: [string, number, number][] = [
  ["1920 × 1080", 1920, 1080],
  ["3840 × 2160", 3840, 2160],
  ["1080 × 1920 (portrait)", 1080, 1920],
  ["2160 × 3840 (portrait)", 2160, 3840],
  ["1280 × 720", 1280, 720],
];

// Number field that commits on blur / Enter, so typing doesn't spam the engine.
function NumberField({ label, value, onCommit, min = 0 }: { label: string; value: number; onCommit: (v: number) => void; min?: number }) {
  const [text, setText] = useState(String(value));
  useEffect(() => setText(String(value)), [value]);
  const commit = () => {
    const v = Math.round(Number(text));
    if (Number.isFinite(v) && v >= min && v !== value) onCommit(v);
    else setText(String(value));
  };
  return (
    <label className="field">
      <span>{label}</span>
      <input
        type="number"
        value={text}
        onChange={(e) => setText(e.target.value)}
        onBlur={commit}
        onKeyDown={(e) => e.key === "Enter" && commit()}
      />
    </label>
  );
}

function TextField({ label, value, onCommit }: { label: string; value: string; onCommit: (v: string) => void }) {
  const [text, setText] = useState(value);
  useEffect(() => setText(value), [value]);
  const commit = () => (text.trim() && text !== value ? onCommit(text.trim()) : setText(value));
  return (
    <label className="field wide">
      <span>{label}</span>
      <input value={text} onChange={(e) => setText(e.target.value)} onBlur={commit} onKeyDown={(e) => e.key === "Enter" && commit()} />
    </label>
  );
}

export function Inspector({
  layout,
  output,
  onPatch,
  onDelete,
}: {
  layout: Layout;
  output: Output;
  onPatch: (patch: Partial<Output>) => void;
  onDelete: () => void;
}) {
  const crop = (k: keyof Rect) => (v: number) => onPatch({ crop: { ...output.crop, [k]: v } });
  const resKey = `${output.width}x${output.height}`;
  const preset = RESOLUTIONS.find(([, w, h]) => `${w}x${h}` === resKey);
  const fitCanvas = () => onPatch({ crop: { x: 0, y: 0, w: layout.canvas.width, h: layout.canvas.height } });
  const matchOutput = () => {
    // Make the crop the same pixel size as the output (1:1), keeping its centre.
    const sideways = output.rotation === 90 || output.rotation === 270;
    const w = sideways ? output.height : output.width;
    const h = sideways ? output.width : output.height;
    const cx = output.crop.x + output.crop.w / 2;
    const cy = output.crop.y + output.crop.h / 2;
    onPatch({ crop: { x: Math.round(cx - w / 2), y: Math.round(cy - h / 2), w, h } });
  };

  return (
    <div className="inspector">
      <div className="inspector-head">
        <h2>{output.name}</h2>
        <label className="toggle">
          <input type="checkbox" checked={output.enabled} onChange={(e) => onPatch({ enabled: e.target.checked })} />
          enabled
        </label>
      </div>

      <LivePreview target={output.id} className="output-preview" />

      <div className="group">
        <TextField label="Name" value={output.name} onCommit={(name) => onPatch({ name })} />
        <TextField label="NDI stream name" value={output.ndiName} onCommit={(ndiName) => onPatch({ ndiName })} />
      </div>

      <div className="group">
        <h3>Output resolution</h3>
        <select
          value={preset ? resKey : "custom"}
          onChange={(e) => {
            const p = RESOLUTIONS.find(([, w, h]) => `${w}x${h}` === e.target.value);
            if (p) onPatch({ width: p[1], height: p[2] });
          }}
        >
          {RESOLUTIONS.map(([label, w, h]) => (
            <option key={label} value={`${w}x${h}`}>
              {label}
            </option>
          ))}
          <option value="custom">Custom</option>
        </select>
        <div className="row">
          <NumberField label="Width" value={output.width} min={16} onCommit={(width) => onPatch({ width })} />
          <NumberField label="Height" value={output.height} min={16} onCommit={(height) => onPatch({ height })} />
        </div>
      </div>

      <div className="group">
        <h3>Source region (crop)</h3>
        <label className="field wide">
          <span>Sample from</span>
          <select value={output.source} onChange={(e) => onPatch({ source: e.target.value })}>
            <option value="canvas">Canvas</option>
            {layout.sources.map((s) => (
              <option key={s.id} value={s.id}>
                Source: {s.name}
              </option>
            ))}
          </select>
        </label>
        <div className="row">
          <NumberField label="X" value={output.crop.x} min={-100000} onCommit={crop("x")} />
          <NumberField label="Y" value={output.crop.y} min={-100000} onCommit={crop("y")} />
        </div>
        <div className="row">
          <NumberField label="W" value={output.crop.w} min={1} onCommit={crop("w")} />
          <NumberField label="H" value={output.crop.h} min={1} onCommit={crop("h")} />
        </div>
        <div className="row buttons">
          <button onClick={matchOutput}>1:1 pixels</button>
          <button onClick={fitCanvas}>Whole canvas</button>
        </div>
      </div>

      <div className="group">
        <h3>Transform</h3>
        <div className="seg">
          {([0, 90, 180, 270] as const).map((r) => (
            <button key={r} className={output.rotation === r ? "on" : ""} onClick={() => onPatch({ rotation: r })}>
              {r}°
            </button>
          ))}
        </div>
        <div className="seg">
          {(["stretch", "fit", "fill"] as const).map((m) => (
            <button key={m} className={output.scaleMode === m ? "on" : ""} onClick={() => onPatch({ scaleMode: m })}>
              {m}
            </button>
          ))}
        </div>
      </div>

      <div className="group">
        <h3>Wire format</h3>
        <div className="seg">
          {(["uyvy", "bgra"] as const).map((f) => (
            <button key={f} className={output.pixelFormat === f ? "on" : ""} onClick={() => onPatch({ pixelFormat: f })}>
              {f === "uyvy" ? "UYVY 4:2:2" : "BGRA"}
            </button>
          ))}
        </div>
        <small className="wide-note">UYVY is NDI's native format and halves readback bandwidth.</small>
      </div>

      <button className="danger" onClick={onDelete}>
        Remove output
      </button>
    </div>
  );
}
