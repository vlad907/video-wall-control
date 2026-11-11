import { useState } from "react";
import { api, type Layout, type Metrics } from "./api";
import { FADES } from "./Panels";

const fmt = (v: number, d = 1) => (Number.isFinite(v) ? v.toFixed(d) : "–");

// One line at the bottom of the window: is the engine keeping up? Click for the full view.
export function StatusBar({ metrics, onOpen }: { metrics: Metrics | null; onOpen: () => void }) {
  if (!metrics) return <footer className="statusbar">Connecting to engine…</footer>;
  const e = metrics.engine;
  const dropped = metrics.outputs.reduce((n, o) => n + (o.droppedNoBuffer ?? 0) + (o.droppedQueue ?? 0), 0);  // display outputs have none
  const slow = e.renderFps < e.targetFps * 0.97;
  const lost = metrics.sources.filter((s) => s.signal === "lost").length;
  return (
    <footer className="statusbar" onClick={onOpen} title="Open the monitor view">
      <span className={`dot ${slow ? "warn" : "on"}`} />
      <span>
        <b className={slow ? "warn" : ""}>{fmt(e.renderFps)}</b> / {e.targetFps} fps
      </span>
      <span>
        GPU <b>{fmt(e.gpuMs, 1)}</b> ms
      </span>
      <span>
        readback <b>{fmt(e.readbackMBps, 0)}</b> MB/s
      </span>
      <span className={dropped ? "warn" : ""}>
        dropped <b>{dropped}</b>
      </span>
      <span>{metrics.outputs.length} outputs</span>
      {lost > 0 && (
        <span className="warn">
          <b className="warn">{lost}</b> input{lost === 1 ? "" : "s"} without signal
        </span>
      )}
      <span className="grow" />
      <span>
        {e.backend} · {e.device} · NDI {e.ndi}
      </span>
    </footer>
  );
}

// The engine's numbers in full: replaces the canvas while the Monitor section is open.
export function MonitorView({ metrics }: { metrics: Metrics | null }) {
  if (!metrics) return <div className="monitor muted">Connecting to engine…</div>;
  const e = metrics.engine;
  const s = metrics.sync;
  const cards: [string, string, boolean?][] = [
    ["Render", `${fmt(e.renderFps)} / ${e.targetFps} fps`, e.renderFps < e.targetFps * 0.97],
    ["GPU time", `${fmt(e.gpuMs, 2)} ms`],
    ["GPU load", e.gpuUtilization >= 0 ? `${fmt(e.gpuUtilization, 0)} %` : "n/a"],
    ["CPU per frame", `${fmt(e.encodeMs, 2)} ms`],
    ["Readback", `${fmt(e.readbackMBps, 0)} MB/s`],
    ["Tick lateness", `${fmt(e.tickJitterMs, 2)} ms`, e.tickJitterMs > 1],
    ["Late frames", String(e.lateFrames), e.lateFrames > 0],
    ["On all outputs", `${fmt(s.completeRatio * 100, 2)} %`, s.completeRatio < 1],
  ];
  return (
    <div className="monitor">
      <div className="monitor-head">
        <h2>Monitor</h2>
        <small className="muted">
          {e.backend} on {e.device} · layout {e.layout} · NDI {e.ndi} · frame {e.frameId}
        </small>
      </div>
      <div className="stat-cards">
        {cards.map(([label, value, bad]) => (
          <div key={label} className={`stat-card${bad ? " bad" : ""}`}>
            <small>{label}</small>
            <b>{value}</b>
          </div>
        ))}
      </div>
      <div className="table-wrap">
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
              <th>Receivers</th>
            </tr>
          </thead>
          <tbody>
            {metrics.outputs.map((o) =>
              o.sink === "display" ? (
                <tr key={o.id} className={o.enabled ? "" : "muted"}>
                  <td>{o.name}</td>
                  <td>{o.display}</td>
                  <td>display</td>
                  <td>–</td>
                  <td className={o.error ? "warn" : ""}>{o.error ? o.error : fmt(o.fps ?? NaN)}</td>
                  <td>{o.presents ?? 0}</td>
                  <td className={o.busy ? "warn" : ""}>{o.busy ?? 0}</td>
                  <td>–</td>
                  <td>{o.repeats ?? 0} repeats</td>
                  <td>{o.refreshHz ? `${o.refreshHz} Hz` : "–"}</td>
                  <td>–</td>
                </tr>
              ) : (
              <tr key={o.id} className={o.enabled ? "" : "muted"}>
                <td>{o.name}</td>
                <td>{o.ndiName}</td>
                <td>{o.sink}</td>
                <td>{o.pixelFormat}</td>
                <td className={o.enabled && o.fps < e.targetFps * 0.97 ? "warn" : ""}>{fmt(o.fps)}</td>
                <td>{o.framesSent}</td>
                <td className={o.droppedNoBuffer + o.droppedQueue ? "warn" : ""}>{o.droppedNoBuffer + o.droppedQueue}</td>
                <td>{o.queueDepth}</td>
                <td>
                  {fmt(o.sendMs, 2)} ({fmt(o.sendMsMax, 1)})
                </td>
                <td>{fmt(o.latencyMs, 1)}</td>
                <td>{o.connections >= 0 ? o.connections : "–"}</td>
              </tr>
              ),
            )}
          </tbody>
        </table>
      </div>
    </div>
  );
}

// Preset strip under the canvas (like a switcher's preset row): one click shows a preset on its group.
export function PresetBar({ layout, onChanged, onError }: { layout: Layout; onChanged: () => void; onError: (e: unknown) => void }) {
  const [fade, setFade] = useState(500);
  const run = (p: Promise<unknown>) => p.then(onChanged).catch(onError);
  const groups = layout.walls.filter((w) => w.presets.length);
  if (!groups.length && !layout.scenes.length)
    return <div className="presetbar empty">Presets appear here once a group has some (group → Content → Save current as preset).</div>;
  return (
    <div className="presetbar">
      <select className="fade" value={fade} onChange={(e) => setFade(+e.target.value)} title="Transition">
        {FADES.map(([label, ms]) => (
          <option key={ms} value={ms}>
            {label}
          </option>
        ))}
      </select>
      {groups.map((w) => (
        <div key={w.id} className="preset-group">
          <span className="preset-group-name">{w.name}</span>
          {w.presets.map((p) => (
            <button key={p.id} className={`preset-tile${p.id === w.activePreset ? " on" : ""}`} onClick={() => run(api.recallGroupPreset(w.id, p.id, fade))} title={`Show "${p.name}" on ${w.name}`}>
              {p.name}
            </button>
          ))}
        </div>
      ))}
      {layout.scenes.length > 0 && (
        <div className="preset-group">
          <span className="preset-group-name">Scenes</span>
          {layout.scenes.map((sc) => (
            <button key={sc.id} className="preset-tile scene" onClick={() => run(api.recallScene(sc.id, fade))} title={`Recall scene "${sc.name}" (whole canvas)`}>
              {sc.name}
            </button>
          ))}
        </div>
      )}
    </div>
  );
}
