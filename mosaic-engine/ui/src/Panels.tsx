import { useEffect, useState } from "react";
import { api, type Layout, type LayoutSummary, type Metrics, type Source } from "./api";
import type { EditMode } from "./CanvasEditor";
import { COLORS } from "./CanvasEditor";

const fmt = (v: number, d = 1) => (Number.isFinite(v) ? v.toFixed(d) : "–");

export function LayoutBar({
  layout,
  mode,
  onMode,
  onChanged,
  onError,
}: {
  layout: Layout;
  mode: EditMode;
  onMode: (m: EditMode) => void;
  onChanged: () => void;
  onError: (e: unknown) => void;
}) {
  const [layouts, setLayouts] = useState<LayoutSummary[]>([]);
  const refresh = () => api.layouts().then(setLayouts).catch(onError);
  useEffect(() => {
    refresh();
  }, [layout.id]); // eslint-disable-line react-hooks/exhaustive-deps

  const run = (p: Promise<unknown>) => p.then(() => (refresh(), onChanged())).catch(onError);
  const presets: [string, number, number][] = [
    ["3840 × 2160", 3840, 2160],
    ["7680 × 4320", 7680, 4320],
    ["15360 × 2160", 15360, 2160],
    ["7680 × 2160", 7680, 2160],
  ];
  const canvasKey = `${layout.canvas.width}x${layout.canvas.height}`;

  return (
    <header className="topbar">
      <div className="brand">Mosaic Editor</div>
      <label className="field inline">
        <span>Layout</span>
        <select value={layout.id} onChange={(e) => run(api.activate(e.target.value))}>
          {layouts.map((l) => (
            <option key={l.id} value={l.id}>
              {l.name}
            </option>
          ))}
        </select>
      </label>
      <button
        onClick={() => {
          const name = prompt("Save the current layout as:", `${layout.name} copy`);
          if (name) run(api.saveAs(name).then((l) => api.activate(l.id)));
        }}
      >
        Save as…
      </button>
      <button
        onClick={() => {
          const others = layouts.filter((l) => !l.active);
          const id = prompt(`Delete which layout?\n${others.map((l) => l.id).join("\n")}`);
          if (id) run(api.deleteLayout(id));
        }}
      >
        Delete…
      </button>
      <div className="seg mode">
        <button className={mode === "outputs" ? "on" : ""} onClick={() => onMode("outputs")} title="Drag the region each display shows">
          Edit outputs
        </button>
        <button className={mode === "sources" ? "on" : ""} onClick={() => onMode("sources")} title="Drag where each input sits on the canvas">
          Edit sources
        </button>
      </div>
      <span className="spacer" />
      <label className="field inline">
        <span>Canvas</span>
        <select
          value={presets.some(([, w, h]) => `${w}x${h}` === canvasKey) ? canvasKey : "custom"}
          onChange={(e) => {
            const p = presets.find(([, w, h]) => `${w}x${h}` === e.target.value);
            if (p) run(api.putLayout({ canvas: { ...layout.canvas, width: p[1], height: p[2] } }));
            else {
              const v = prompt("Canvas size (W x H):", `${layout.canvas.width}x${layout.canvas.height}`);
              const m = v?.match(/(\d+)\s*[x×]\s*(\d+)/);
              if (m) run(api.putLayout({ canvas: { ...layout.canvas, width: +m[1], height: +m[2] } }));
            }
          }}
        >
          {presets.map(([label, w, h]) => (
            <option key={label} value={`${w}x${h}`}>
              {label}
            </option>
          ))}
          <option value="custom">{presets.some(([, w, h]) => `${w}x${h}` === canvasKey) ? "Custom…" : `${layout.canvas.width} × ${layout.canvas.height}`}</option>
        </select>
      </label>
      <label className="field inline">
        <span>FPS</span>
        <select value={layout.frameRate} onChange={(e) => run(api.putLayout({ frameRate: +e.target.value }))}>
          {[24, 25, 30, 50, 59.94, 60].map((f) => (
            <option key={f} value={f}>
              {f}
            </option>
          ))}
        </select>
      </label>
    </header>
  );
}

export function OutputList({
  layout,
  selectedId,
  onSelect,
  onAdd,
  onWall,
  onToggle,
}: {
  layout: Layout;
  selectedId: string | null;
  onSelect: (id: string) => void;
  onAdd: () => void;
  onWall: () => void;
  onToggle: (id: string, enabled: boolean) => void;
}) {
  const wall = layout.wall;
  return (
    <section className="list">
      <div className="list-head">
        <h3>Outputs ({layout.outputs.length})</h3>
        <span>
          <button onClick={onWall} title="Generate a grid of panels with bezel gaps">
            Wall…
          </button>{" "}
          <button onClick={onAdd}>+ Add</button>
        </span>
      </div>
      {wall && (
        <div className="group-head" onClick={onWall}>
          Wall {wall.cols} × {wall.rows} · {wall.outputWidth}×{wall.outputHeight}
          {wall.rotation ? ` ↻${wall.rotation}°` : ""} · gaps {wall.gapX}/{wall.gapY} px
        </div>
      )}
      {layout.outputs.map((o, i) => (
        <div key={o.id} className={`list-item${o.id === selectedId ? " selected" : ""}${o.group ? " grouped" : ""}`} onClick={() => onSelect(o.id)}>
          <span className="swatch" style={{ background: COLORS[i % COLORS.length] }} />
          <div className="list-text">
            <div>{o.name}</div>
            <small>
              {o.ndiName} · {o.width}×{o.height}
              {o.rotation ? ` · ${o.rotation}°` : ""}
            </small>
          </div>
          <input
            type="checkbox"
            title="enabled"
            checked={o.enabled}
            onClick={(e) => e.stopPropagation()}
            onChange={(e) => onToggle(o.id, e.target.checked)}
          />
        </div>
      ))}
    </section>
  );
}

export function SourcesPanel({
  layout,
  selectedId,
  onSelect,
  onChanged,
  onError,
}: {
  layout: Layout;
  selectedId: string | null;
  onSelect: (id: string) => void;
  onChanged: () => void;
  onError: (e: unknown) => void;
}) {
  const [info, setInfo] = useState<{ sources: Source[]; ndi: { available: boolean; discovered: string[] } } | null>(null);
  const refresh = () => api.sources().then(setInfo).catch(onError);
  useEffect(() => {
    refresh();
    const t = window.setInterval(refresh, 3000);
    return () => window.clearInterval(t);
  }, [layout]); // eslint-disable-line react-hooks/exhaustive-deps

  const run = (p: Promise<unknown>) => p.then(() => (refresh(), onChanged())).catch(onError);
  const add = (s: Partial<Source>) => api.addSource({ rect: { x: 0, y: 0, w: layout.canvas.width, h: layout.canvas.height }, ...s }).then((c) => (onSelect(c.id), refresh(), onChanged())).catch(onError);
  const status = (id: string) => info?.sources.find((s) => s.id === id)?.status;

  return (
    <section className="list">
      <div className="list-head">
        <h3>Sources (top last)</h3>
        <select
          value=""
          onChange={(e) => {
            if (e.target.value === "pattern") add({ name: "Test pattern", type: "test_pattern" });
            if (e.target.value === "solid") add({ name: "Black", type: "solid", color: [0, 0, 0, 1] });
          }}
        >
          <option value="">+ Add…</option>
          <option value="pattern">Test pattern</option>
          <option value="solid">Solid colour</option>
        </select>
      </div>
      {layout.sources.map((s) => {
        const st = status(s.id);
        return (
          <div key={s.id} className={`list-item${s.id === selectedId ? " selected" : ""}`} onClick={() => onSelect(s.id)}>
            <div className="list-text">
              <div>
                {s.name} <small>({s.type === "ndi" ? "NDI" : s.type === "solid" ? "colour" : "pattern"})</small>
              </div>
              <small>
                {s.opacity < 1 ? `${Math.round(s.opacity * 100)}% · ` : ""}
                {st?.status ?? "…"}
                {st?.inputFps ? ` · ${fmt(st.inputFps)} fps` : ""}
              </small>
            </div>
            <input
              type="checkbox"
              title="enabled"
              checked={s.enabled}
              onClick={(e) => e.stopPropagation()}
              onChange={(e) => run(api.patchSource(s.id, { enabled: e.target.checked }))}
            />
          </div>
        );
      })}
      <div className="ndi-add">
        {info && !info.ndi.available && <small className="muted">NDI unavailable in this build (see docs/NDI.md)</small>}
        {info?.ndi.available && (
          <select value="" onChange={(e) => e.target.value && add({ name: e.target.value, type: "ndi", ndiName: e.target.value })}>
            <option value="">+ Add NDI source ({info.ndi.discovered.length} found)…</option>
            {info.ndi.discovered.map((n) => (
              <option key={n} value={n}>
                {n}
              </option>
            ))}
          </select>
        )}
      </div>
    </section>
  );
}

const FADES: [string, number][] = [
  ["Cut", 0],
  ["0.5 s", 500],
  ["1 s", 1000],
  ["2 s", 2000],
];

// Saved source arrangements. Recalling one swaps the sources, optionally crossfading.
export function ScenesPanel({ layout, onChanged, onError }: { layout: Layout; onChanged: () => void; onError: (e: unknown) => void }) {
  const [fade, setFade] = useState(1000);
  const run = (p: Promise<unknown>) => p.then(onChanged).catch(onError);
  const same = (a: unknown, b: unknown) => JSON.stringify(a) === JSON.stringify(b);
  return (
    <section className="list">
      <div className="list-head">
        <h3>Scenes</h3>
        <button
          onClick={() => {
            const name = prompt("Save the current sources as scene:", `Scene ${layout.scenes.length + 1}`);
            if (name) run(api.saveScene(name));
          }}
        >
          + Save
        </button>
      </div>
      {layout.scenes.length === 0 && <small className="muted">Save the current sources to switch back to them later.</small>}
      {layout.scenes.map((sc) => (
        <div key={sc.id} className={`list-item${same(sc.sources, layout.sources) ? " selected" : ""}`}>
          <div className="list-text">
            <div>{sc.name}</div>
            <small>{sc.sources.length} source(s)</small>
          </div>
          <button title="Recall" onClick={() => run(api.recallScene(sc.id, fade))}>
            ▶
          </button>
          <button title="Overwrite with the current sources" onClick={() => confirm(`Overwrite "${sc.name}"?`) && run(api.saveScene(sc.name))}>
            ⟳
          </button>
          <button title="Delete" onClick={() => confirm(`Delete "${sc.name}"?`) && run(api.deleteScene(sc.id))}>
            ✕
          </button>
        </div>
      ))}
      {layout.scenes.length > 0 && (
        <label className="field inline">
          <span>Transition</span>
          <select value={fade} onChange={(e) => setFade(+e.target.value)}>
            {FADES.map(([label, ms]) => (
              <option key={ms} value={ms}>
                {label}
              </option>
            ))}
          </select>
        </label>
      )}
    </section>
  );
}

export function MetricsPanel({ metrics }: { metrics: Metrics | null }) {
  if (!metrics) return <footer className="metrics">connecting to engine…</footer>;
  const e = metrics.engine;
  const s = metrics.sync;
  return (
    <footer className="metrics">
      <div className="engine-stats">
        <span>
          <b>{e.backend}</b> {e.device}
        </span>
        <span className={e.renderFps < e.targetFps * 0.97 ? "warn" : ""}>
          render <b>{fmt(e.renderFps)}</b>/{e.targetFps} fps
        </span>
        <span>
          CPU encode <b>{fmt(e.encodeMs, 2)}</b> ms
        </span>
        <span>
          GPU <b>{fmt(e.gpuMs, 2)}</b> ms
        </span>
        <span>
          GPU util <b>{e.gpuUtilization >= 0 ? `${fmt(e.gpuUtilization, 0)}%` : "n/a"}</b>
        </span>
        <span>
          readback <b>{fmt(e.readbackMBps, 0)}</b> MB/s
        </span>
        <span className={e.tickJitterMs > 1 ? "warn" : ""}>
          tick lateness <b>{fmt(e.tickJitterMs, 2)}</b> ms
        </span>
        <span className={e.lateFrames ? "warn" : ""}>
          late <b>{e.lateFrames}</b>
        </span>
        <span>
          frame <b>{e.frameId}</b>
        </span>
        <span className={s.completeRatio < 1 ? "warn" : ""}>
          frames on all outputs <b>{fmt(s.completeRatio * 100, 2)}%</b> ({s.framesPartial} partial)
        </span>
        <span>NDI: {e.ndi}</span>
      </div>
      <table>
        <thead>
          <tr>
            <th>Output</th>
            <th>Stream</th>
            <th>Sink</th>
            <th>Format</th>
            <th>FPS</th>
            <th>Sent</th>
            <th>Dropped</th>
            <th>Queue</th>
            <th>Submit ms (max)</th>
            <th>Latency ms</th>
            <th>Last frame</th>
            <th>Rx</th>
          </tr>
        </thead>
        <tbody>
          {metrics.outputs.map((o) => (
            <tr key={o.id} className={o.enabled ? "" : "muted"}>
              <td>{o.name}</td>
              <td>{o.ndiName}</td>
              <td>{o.sink}</td>
              <td>{o.pixelFormat}</td>
              <td className={o.enabled && o.fps < e.targetFps * 0.97 ? "warn" : ""}>{fmt(o.fps)}</td>
              <td>{o.framesSent}</td>
              <td className={o.droppedNoBuffer + o.droppedQueue ? "warn" : ""}>
                {o.droppedNoBuffer + o.droppedQueue}
              </td>
              <td>{o.queueDepth}</td>
              <td>
                {fmt(o.sendMs, 2)} ({fmt(o.sendMsMax, 1)})
              </td>
              <td>{fmt(o.latencyMs, 1)}</td>
              <td>{o.lastFrameId}</td>
              <td>{o.connections >= 0 ? o.connections : "–"}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </footer>
  );
}
