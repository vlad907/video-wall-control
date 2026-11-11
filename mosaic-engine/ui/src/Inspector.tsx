import { useEffect, useState } from "react";
import { api, NEUTRAL_COLOR, type ColorAdjust, type GpuInfo, type Layout, type Output, type Rect } from "./api";

// One colour-correction slider: commits when released (each change re-renders the output).
function ColorSlider({ label, value, min, max, step, onCommit }: { label: string; value: number; min: number; max: number; step: number; onCommit: (v: number) => void }) {
  const [v, setV] = useState(value);
  useEffect(() => setV(value), [value]);
  const commit = () => v !== value && onCommit(v);
  return (
    <label className="field wide color-slider">
      <span>
        {label} <b>{v.toFixed(2)}</b>
      </span>
      <input
        type="range"
        min={min}
        max={max}
        step={step}
        value={v}
        onChange={(e) => setV(+e.target.value)}
        onPointerUp={commit}
        onKeyUp={commit}
        onDoubleClick={() => onCommit((NEUTRAL_COLOR as unknown as Record<string, number>)[label.toLowerCase()] ?? v)}
      />
    </label>
  );
}
import { LivePreview } from "./LivePreview";

const RESOLUTIONS: [string, number, number][] = [
  ["1920 × 1080", 1920, 1080],
  ["3840 × 2160", 3840, 2160],
  ["1080 × 1920 (portrait)", 1080, 1920],
  ["2160 × 3840 (portrait)", 2160, 3840],
  ["1280 × 720", 1280, 720],
];

// Number field that commits on blur / Enter, so typing doesn't spam the engine.
export function NumberField({
  label,
  value,
  onCommit,
  min = 0,
  decimals = 0,
}: {
  label: string;
  value: number;
  onCommit: (v: number) => void;
  min?: number;
  decimals?: number;
}) {
  const [text, setText] = useState(String(value));
  useEffect(() => setText(String(value)), [value]);
  const commit = () => {
    const f = 10 ** decimals;
    const v = Math.round(Number(text) * f) / f;
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

export function TextField({ label, value, onCommit }: { label: string; value: string; onCommit: (v: string) => void }) {
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
  shownOn = [],
  onBezel,
}: {
  layout: Layout;
  output: Output;
  onPatch: (patch: Partial<Output>) => void;
  onDelete: () => void;
  shownOn?: string[]; // "Endpoint · Display N" for each endpoint display showing it
  onBezel?: () => void; // selects its whole group for bezel correction
}) {
  const crop = (k: keyof Rect) => (v: number) => onPatch({ crop: { ...output.crop, [k]: v } });
  const [gpus, setGpus] = useState<GpuInfo[]>([]);
  useEffect(() => {
    api.gpus().then(setGpus).catch(() => {});
  }, []);
  // Monitors this output can go to: those on the canvas's GPU (any GPU's while it's on the default one).
  const monitors = gpus.flatMap((g) => (layout.gpu === undefined || layout.gpu < 0 || g.index === layout.gpu ? g.outputs.map((o) => ({ gpu: g, ...o })) : []));
  const monitorLabel = (name: string) => name.replace(/^\\\\\.\\/, "");
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

      <small className="muted">{shownOn.length ? `Shown on ${shownOn.join(", ")}` : "Not on any endpoint display yet"}</small>
      <LivePreview target={output.id} className="output-preview" />
      {onBezel && (
        <button onClick={onBezel} title="Select all displays of this group and correct for their bezels">
          Bezel correction for its group…
        </button>
      )}

      <div className="group">
        <TextField label="Name" value={output.name} onCommit={(name) => onPatch({ name })} />
        <label className="field wide" title="NDI: a stream receiving PCs show. A monitor: full screen on a connector of this canvas's GPU (DisplayPort / HDMI to a video wall or its controller).">
          <span>Send to</span>
          <select value={output.display ?? ""} onChange={(e) => onPatch({ display: e.target.value })}>
            <option value="">NDI stream (receiving PCs)</option>
            {monitors.map((m) => (
              <option key={m.name} value={m.name}>
                Monitor {monitorLabel(m.name)} · {m.width}×{m.height}
                {m.refreshHz ? ` ${m.refreshHz} Hz` : ""}
                {gpus.length > 1 ? ` · ${m.gpu.name}` : ""}
              </option>
            ))}
            {output.display && !monitors.some((m) => m.name === output.display) && <option value={output.display}>Monitor {monitorLabel(output.display)} (not found)</option>}
          </select>
        </label>
        {output.display ? (
          <small className="muted">Drawn at the monitor's resolution, on every refresh of it. The output resolution below only matters for NDI.</small>
        ) : (
          <TextField label="NDI stream name" value={output.ndiName} onCommit={(ndiName) => onPatch({ ndiName })} />
        )}
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
        <NumberField label="Tilt (° clockwise)" value={+(output.angle ?? 0).toFixed(2)} min={-360} decimals={2} onCommit={(angle) => onPatch({ angle })} />
        <div className="row buttons">
          <button onClick={() => onPatch({ angle: Math.round(((output.angle ?? 0) - 0.5) * 100) / 100 })}>↺ 0.5°</button>
          <button onClick={() => onPatch({ angle: Math.round(((output.angle ?? 0) + 0.5) * 100) / 100 })}>↻ 0.5°</button>
        </div>
        <small className="wide-note muted">Tilt matches a panel mounted at an angle; mount rotation above is for portrait panels.</small>
        <div className="seg">
          {(["stretch", "fit", "fill"] as const).map((m) => (
            <button key={m} className={output.scaleMode === m ? "on" : ""} onClick={() => onPatch({ scaleMode: m })}>
              {m}
            </button>
          ))}
        </div>
      </div>

      <div className="group">
        <h3>Colour</h3>
        {(() => {
          const c: ColorAdjust = { ...NEUTRAL_COLOR, ...(output.color ?? {}) };
          const set = (k: keyof ColorAdjust) => (v: number) => onPatch({ color: { ...c, [k]: v } });
          return (
            <>
              <ColorSlider label="Brightness" value={c.brightness} min={-0.5} max={0.5} step={0.01} onCommit={set("brightness")} />
              <ColorSlider label="Contrast" value={c.contrast} min={0} max={2} step={0.01} onCommit={set("contrast")} />
              <ColorSlider label="Saturation" value={c.saturation} min={0} max={2} step={0.01} onCommit={set("saturation")} />
              <ColorSlider label="Gamma" value={c.gamma} min={0.5} max={2} step={0.01} onCommit={set("gamma")} />
              <ColorSlider label="Red" value={c.red} min={0} max={1.5} step={0.01} onCommit={set("red")} />
              <ColorSlider label="Green" value={c.green} min={0} max={1.5} step={0.01} onCommit={set("green")} />
              <ColorSlider label="Blue" value={c.blue} min={0} max={1.5} step={0.01} onCommit={set("blue")} />
              <div className="row buttons">
                <button disabled={!output.color} onClick={() => onPatch({ color: null as unknown as ColorAdjust })}>
                  Reset colour
                </button>
              </div>
              <small className="wide-note muted">Matches this display to the others. Double-click a slider to reset it. Applied in the engine, so it works for NDI and monitor outputs alike.</small>
            </>
          );
        })()}
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
