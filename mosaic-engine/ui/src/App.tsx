import { useCallback, useEffect, useRef, useState } from "react";
import { api, type Layout, type Metrics, type Output, type Rect, type Source } from "./api";
import { CanvasEditor, type EditMode } from "./CanvasEditor";
import { Inspector } from "./Inspector";
import { LayoutBar, MetricsPanel, OutputList, ScenesPanel, SourcesPanel } from "./Panels";
import { SourceInspector } from "./SourceInspector";
import { WallPanel } from "./WallPanel";

export function App() {
  const [layout, setLayout] = useState<Layout | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null); // output id
  const [sourceId, setSourceId] = useState<string | null>(null);
  const [mode, setMode] = useState<EditMode>("outputs");
  const [wallOpen, setWallOpen] = useState(false);
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

  // Drag edits are throttled to ~20 updates/s; the final one always goes out.
  const pending = useRef<{ kind: EditMode; id: string; rect: Rect } | null>(null);
  const lastSent = useRef(0);
  const timer = useRef<number | undefined>(undefined);
  const flush = useCallback(() => {
    const p = pending.current;
    pending.current = null;
    lastSent.current = performance.now();
    if (!p) return;
    if (p.kind === "outputs") api.patchOutput(p.id, { crop: p.rect }).catch(onError);
    else api.patchSource(p.id, { rect: p.rect }).catch(onError);
  }, [onError]);

  const onRectChange = useCallback(
    (kind: EditMode, id: string, rect: Rect, final: boolean) => {
      setLayout((l) =>
        !l
          ? l
          : kind === "outputs"
            ? { ...l, outputs: l.outputs.map((o) => (o.id === id ? { ...o, crop: rect } : o)) }
            : { ...l, sources: l.sources.map((s) => (s.id === id ? { ...s, rect } : s)) },
      );
      pending.current = { kind, id, rect };
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

  const patchSource = useCallback(
    (id: string, p: Partial<Source>) => {
      setLayout((l) => (l ? { ...l, sources: l.sources.map((s) => (s.id === id ? { ...s, ...p } : s)) } : l));
      api
        .patchSource(id, p)
        .then(reload)
        .catch((e) => (onError(e), reload()));
    },
    [onError, reload],
  );

  const selectOutput = useCallback((id: string | null) => {
    setSelectedId(id);
    if (id) (setMode("outputs"), setWallOpen(false));
  }, []);
  const selectSource = useCallback((id: string | null) => {
    setSourceId(id);
    if (id) (setMode("sources"), setWallOpen(false));
  }, []);

  // Arrow keys nudge the selected rectangle (Shift = 10 px).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement).tagName;
      if (!layout || tag === "INPUT" || tag === "SELECT") return;
      const d = e.shiftKey ? 10 : 1;
      const delta: Record<string, [number, number]> = { ArrowLeft: [-d, 0], ArrowRight: [d, 0], ArrowUp: [0, -d], ArrowDown: [0, d] };
      if (!delta[e.key]) return;
      const [dx, dy] = delta[e.key];
      const r =
        mode === "outputs" ? layout.outputs.find((x) => x.id === selectedId)?.crop : layout.sources.find((x) => x.id === sourceId)?.rect;
      const id = mode === "outputs" ? selectedId : sourceId;
      if (!r || !id) return;
      e.preventDefault();
      onRectChange(mode, id, { ...r, x: r.x + dx, y: r.y + dy }, true);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [layout, mode, selectedId, sourceId, onRectChange]);

  const onCanvasRect = useCallback((id: string, rect: Rect, final: boolean) => onRectChange(mode, id, rect, final), [mode, onRectChange]);

  if (!layout) return <div className="loading">{error ?? "Connecting to engine…"}</div>;
  const selected = layout.outputs.find((o) => o.id === selectedId) ?? null;
  const selectedSource = layout.sources.find((s) => s.id === sourceId) ?? null;

  const moveSource = (id: string, delta: -1 | 1) => {
    const list = [...layout.sources];
    const i = list.findIndex((s) => s.id === id);
    const j = i + delta;
    if (i < 0 || j < 0 || j >= list.length) return;
    [list[i], list[j]] = [list[j], list[i]];
    setLayout({ ...layout, sources: list });
    api.putLayout({ sources: list }).then(reload).catch(onError);
  };

  let right: React.ReactNode;
  if (wallOpen) {
    right = (
      <WallPanel
        layout={layout}
        onClose={() => setWallOpen(false)}
        onApply={(w) =>
          api
            .putWall(w)
            .then((l) => (setLayout(l), setWallOpen(false)))
            .catch(onError)
        }
        onRemove={() =>
          api
            .deleteWall()
            .then(() => (reload(), setWallOpen(false)))
            .catch(onError)
        }
      />
    );
  } else if (mode === "sources" && selectedSource) {
    right = (
      <SourceInspector
        layout={layout}
        source={{ ...selectedSource, status: metrics?.sources.find((x) => x.id === selectedSource.id) }}
        onPatch={(p) => patchSource(selectedSource.id, p)}
        onMove={(d) => moveSource(selectedSource.id, d)}
        onDelete={() => {
          if (!confirm(`Remove ${selectedSource.name}?`)) return;
          api
            .deleteSource(selectedSource.id)
            .then(() => (setSourceId(null), reload()))
            .catch(onError);
        }}
      />
    );
  } else if (mode === "outputs" && selected) {
    right = (
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
    );
  } else {
    right = (
      <div className="hint">
        {mode === "outputs" ? "Select an output on the canvas or in the list to edit it." : "Select a source on the canvas or in the list to edit it."}
      </div>
    );
  }

  const addOutput = () => {
    const n = layout.outputs.length;
    const w = Math.min(1920, layout.canvas.width);
    const h = Math.min(1080, layout.canvas.height);
    api
      .addOutput({ name: `Display ${n + 1}`, width: 1920, height: 1080, crop: { x: (n * 120) % Math.max(1, layout.canvas.width - w), y: (n * 120) % Math.max(1, layout.canvas.height - h), w, h } })
      .then((o) => (selectOutput(o.id), reload()))
      .catch(onError);
  };

  return (
    <div className="app">
      <LayoutBar layout={layout} mode={mode} onMode={setMode} onChanged={reload} onError={onError} />
      {error && (
        <div className="error" onClick={() => setError(null)}>
          {error} <small>(click to dismiss)</small>
        </div>
      )}
      <aside className="sidebar">
        <OutputList
          layout={layout}
          selectedId={mode === "outputs" ? selectedId : null}
          onSelect={selectOutput}
          onAdd={addOutput}
          onWall={() => setWallOpen(true)}
          onToggle={(id, enabled) => patch(id, { enabled })}
        />
        <SourcesPanel layout={layout} selectedId={mode === "sources" ? sourceId : null} onSelect={selectSource} onChanged={reload} onError={onError} />
        <ScenesPanel layout={layout} onChanged={reload} onError={onError} />
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
          Canvas {layout.canvas.width} × {layout.canvas.height} ·{" "}
          {mode === "outputs" ? "editing what each display shows" : "editing where each source sits"} · drag to move, drag handles to resize, arrow keys nudge
        </div>
        <CanvasEditor
          layout={layout}
          mode={mode}
          selectedId={mode === "outputs" ? selectedId : sourceId}
          snap={snap}
          onSelect={mode === "outputs" ? selectOutput : selectSource}
          onRectChange={onCanvasRect}
        />
      </main>
      <aside className="right">{right}</aside>
      <MetricsPanel metrics={metrics} />
    </div>
  );
}
