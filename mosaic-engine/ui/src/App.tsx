import { useCallback, useEffect, useRef, useState } from "react";
import { api, type Layout, type Metrics, type Output, type Rect } from "./api";
import { CanvasEditor } from "./CanvasEditor";
import { Inspector } from "./Inspector";
import { LayoutBar, MetricsPanel, OutputList, SourcesPanel } from "./Panels";

export function App() {
  const [layout, setLayout] = useState<Layout | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null);
  const [metrics, setMetrics] = useState<Metrics | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [snap, setSnap] = useState(10);
  const versionRef = useRef<number>(-1);

  const reload = useCallback(() => api.layout().then(setLayout).catch((e) => setError(String(e))), []);
  const onError = useCallback((e: unknown) => setError(e instanceof Error ? e.message : String(e)), []);

  useEffect(() => {
    reload();
  }, [reload]);

  // Live metrics; a layout version change (from any client) triggers a reload.
  useEffect(() => {
    const es = new EventSource("/api/events");
    es.onmessage = (ev) => {
      const data = JSON.parse(ev.data) as { metrics: Metrics; layoutVersion: number };
      setMetrics(data.metrics);
      if (versionRef.current !== -1 && data.layoutVersion !== versionRef.current) reload();
      versionRef.current = data.layoutVersion;
    };
    es.onerror = () => setMetrics(null);
    return () => es.close();
  }, [reload]);

  // Crop edits while dragging are throttled to ~20 updates/s; the final one always goes out.
  const pending = useRef<{ id: string; crop: Rect } | null>(null);
  const lastSent = useRef(0);
  const timer = useRef<number | undefined>(undefined);
  const flush = useCallback(() => {
    const p = pending.current;
    pending.current = null;
    lastSent.current = performance.now();
    if (p) api.patchOutput(p.id, { crop: p.crop }).catch(onError);
  }, [onError]);

  const onCropChange = useCallback(
    (id: string, crop: Rect, final: boolean) => {
      setLayout((l) => (l ? { ...l, outputs: l.outputs.map((o) => (o.id === id ? { ...o, crop } : o)) } : l));
      pending.current = { id, crop };
      window.clearTimeout(timer.current);
      const wait = 50 - (performance.now() - lastSent.current);
      if (final || wait <= 0) flush();
      else timer.current = window.setTimeout(flush, wait);
    },
    [flush],
  );

  const patch = useCallback(
    (id: string, p: Partial<Output>) => {
      setLayout((l) => (l ? { ...l, outputs: l.outputs.map((o) => (o.id === id ? { ...o, ...p } : o)) } : l));
      api
        .patchOutput(id, p)
        .then(reload)
        .catch((e) => (onError(e), reload()));
    },
    [onError, reload],
  );

  // Arrow keys nudge the selected output (Shift = 10 px).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (!layout || !selectedId || (e.target as HTMLElement).tagName === "INPUT") return;
      const o = layout.outputs.find((x) => x.id === selectedId);
      const d = e.shiftKey ? 10 : 1;
      const delta: Record<string, [number, number]> = { ArrowLeft: [-d, 0], ArrowRight: [d, 0], ArrowUp: [0, -d], ArrowDown: [0, d] };
      if (!o || !delta[e.key]) return;
      e.preventDefault();
      const [dx, dy] = delta[e.key];
      onCropChange(o.id, { ...o.crop, x: o.crop.x + dx, y: o.crop.y + dy }, true);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [layout, selectedId, onCropChange]);

  if (!layout) return <div className="loading">{error ?? "Connecting to engine…"}</div>;
  const selected = layout.outputs.find((o) => o.id === selectedId) ?? null;

  const addOutput = () => {
    const n = layout.outputs.length;
    const w = Math.min(1920, layout.canvas.width);
    const h = Math.min(1080, layout.canvas.height);
    api
      .addOutput({ name: `Display ${n + 1}`, width: 1920, height: 1080, crop: { x: (n * 120) % Math.max(1, layout.canvas.width - w), y: (n * 120) % Math.max(1, layout.canvas.height - h), w, h } })
      .then((o) => (setSelectedId(o.id), reload()))
      .catch(onError);
  };

  return (
    <div className="app">
      <LayoutBar layout={layout} onChanged={reload} onError={onError} />
      {error && (
        <div className="error" onClick={() => setError(null)}>
          {error} <small>(click to dismiss)</small>
        </div>
      )}
      <aside className="sidebar">
        <OutputList
          layout={layout}
          selectedId={selectedId}
          onSelect={setSelectedId}
          onAdd={addOutput}
          onToggle={(id, enabled) => patch(id, { enabled })}
        />
        <SourcesPanel layout={layout} onChanged={reload} onError={onError} />
        <label className="field inline snap">
          <span>Snap</span>
          <select value={snap} onChange={(e) => setSnap(+e.target.value)}>
            {[1, 10, 60, 120, 240].map((s) => (
              <option key={s} value={s}>
                {s === 1 ? "off (1 px)" : `${s} px`}
              </option>
            ))}
          </select>
        </label>
      </aside>
      <main className="main">
        <div className="canvas-info">
          Canvas {layout.canvas.width} × {layout.canvas.height} · drag a region to move it, drag its handles to resize, arrow keys nudge
        </div>
        <CanvasEditor layout={layout} selectedId={selectedId} snap={snap} onSelect={setSelectedId} onCropChange={onCropChange} />
      </main>
      <aside className="right">
        {selected ? (
          <Inspector
            layout={layout}
            output={selected}
            onPatch={(p) => patch(selected.id, p)}
            onDelete={() => {
              if (!confirm(`Remove ${selected.name}?`)) return;
              api
                .deleteOutput(selected.id)
                .then(() => (setSelectedId(null), reload()))
                .catch(onError);
            }}
          />
        ) : (
          <div className="hint">Select an output on the canvas or in the list to edit it.</div>
        )}
      </aside>
      <MetricsPanel metrics={metrics} />
    </div>
  );
}
