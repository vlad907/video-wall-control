import { useCallback, useEffect, useRef, useState } from "react";
import { api, type EndpointsInfo, type Layout, type Metrics, type Output, type Rect, type Screen, type Source } from "./api";
import { CanvasEditor, type EditMode } from "./CanvasEditor";
import { EndpointList, EndpointPanel } from "./Endpoints";
import { Inspector } from "./Inspector";
import { LayoutBar, MetricsPanel, OutputList, ScenesPanel, SourcesPanel } from "./Panels";
import { SourceInspector } from "./SourceInspector";
import { WallPanel } from "./WallPanel";

export function App() {
  const [layout, setLayout] = useState<Layout | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null); // output id
  const [sourceId, setSourceId] = useState<string | null>(null);
  const [mode, setMode] = useState<EditMode>("outputs");
  const [wallEdit, setWallEdit] = useState<{ id: string | null } | null>(null); // id null = new group
  const [endpointId, setEndpointId] = useState<string | null>(null);
  const [endpoints, setEndpoints] = useState<EndpointsInfo | null>(null);
  const [metrics, setMetrics] = useState<Metrics | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [snap, setSnap] = useState(10);
  const [smartSnap, setSmartSnap] = useState(() => {
    try {
      return localStorage.getItem("mosaic.smartSnap") !== "off";
    } catch {
      return true;
    }
  });
  const toggleSmartSnap = (on: boolean) => {
    setSmartSnap(on);
    try {
      localStorage.setItem("mosaic.smartSnap", on ? "on" : "off");
    } catch {
      /* per-browser preference only */
    }
  };
  const versionRef = useRef<number>(-1);

  const reload = useCallback(() => api.layout().then(setLayout).catch((e) => setError(String(e))), []);
  const onError = useCallback((e: unknown) => setError(e instanceof Error ? e.message : String(e)), []);

  useEffect(() => {
    reload();
  }, [reload]);

  // Endpoint health changes without layout changes (PCs check in every few seconds).
  const loadEndpoints = useCallback(() => api.endpoints().then(setEndpoints).catch(() => {}), []);
  useEffect(() => {
    loadEndpoints();
    const t = window.setInterval(loadEndpoints, 3000);
    return () => window.clearInterval(t);
  }, [loadEndpoints, layout]);

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
  const pending = useRef<{ kind: EditMode; id: string; rect?: Rect; angle?: number } | null>(null);
  const lastSent = useRef(0);
  const timer = useRef<number | undefined>(undefined);
  const flush = useCallback(() => {
    const p = pending.current;
    pending.current = null;
    lastSent.current = performance.now();
    if (!p) return;
    if (p.angle !== undefined) api.patchOutput(p.id, { angle: p.angle }).catch(onError);
    else if (p.kind === "outputs") api.patchOutput(p.id, { crop: p.rect }).catch(onError);
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

  const onAngleChange = useCallback(
    (id: string, angle: number, final: boolean) => {
      setLayout((l) => (l ? { ...l, outputs: l.outputs.map((o) => (o.id === id ? { ...o, angle } : o)) } : l));
      pending.current = { kind: "outputs", id, angle };
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
    if (id) (setMode("outputs"), setWallEdit(null), setEndpointId(null));
  }, []);
  const selectSource = useCallback((id: string | null) => {
    setSourceId(id);
    if (id) (setMode("sources"), setWallEdit(null), setEndpointId(null));
  }, []);

  // Arrow keys nudge the selected rectangle (Shift = 10 px); [ and ] tilt an output (0.1°, Shift 1°).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const tag = (e.target as HTMLElement).tagName;
      if (!layout || tag === "INPUT" || tag === "SELECT") return;
      const tilt = e.code === "BracketLeft" ? -1 : e.code === "BracketRight" ? 1 : 0;
      const out = layout.outputs.find((x) => x.id === selectedId);
      if (tilt && mode === "outputs" && out) {
        e.preventDefault();
        const a = Math.round(((out.angle ?? 0) + tilt * (e.shiftKey ? 1 : 0.1)) * 10) / 10;
        onAngleChange(out.id, a, true);
        return;
      }
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
  }, [layout, mode, selectedId, sourceId, onRectChange, onAngleChange]);

  const onCanvasRect = useCallback((id: string, rect: Rect, final: boolean) => onRectChange(mode, id, rect, final), [mode, onRectChange]);
  const onGroupMove = useCallback(
    (id: string, x: number, y: number) =>
      api
        .moveWall(id, x, y)
        .then(setLayout)
        .catch((e) => (onError(e), reload())),
    [onError, reload],
  );

  if (!layout) return <div className="loading">{error ?? "Connecting to engine…"}</div>;
  const selected = layout.outputs.find((o) => o.id === selectedId) ?? null;
  const selectedSource = layout.sources.find((s) => s.id === sourceId) ?? null;

  // Layer order: layout.sources is bottom first, so the last one is the top (primary) layer.
  const reorderSource = (id: string, toIndex: number) => {
    const list = [...layout.sources];
    const i = list.findIndex((s) => s.id === id);
    if (i < 0 || i === toIndex || toIndex < 0 || toIndex >= list.length) return;
    const [moved] = list.splice(i, 1);
    list.splice(toIndex, 0, moved);
    setLayout({ ...layout, sources: list });
    api.putLayout({ sources: list }).then(reload).catch(onError);
  };

  // A PC that checked in (or a name typed in): one display slot per screen, the main screen left
  // out when there are others (it's usually the control screen).
  const setUpEndpoint = (host: string, screens: Screen[]) => {
    const pcName = host || prompt("PC name of the receiving PC (as shown in its dashboard):")?.trim();
    if (!pcName) return;
    const name = prompt("Name for this endpoint:", pcName)?.trim();
    if (!name) return;
    const usable = screens.length > 1 ? screens.filter((s) => !s.primary) : screens;
    api
      .createEndpoint({ name, host: pcName, displays: usable.map((s) => ({ screen: s.device, output: "" })) })
      .then((e) => (setEndpointId(e.id), setWallEdit(null), reload(), loadEndpoints()))
      .catch(onError);
  };
  const selectedEndpoint = endpoints?.endpoints.find((e) => e.id === endpointId) ?? null;
  const shownOn = (outputId: string) =>
    (endpoints?.endpoints ?? []).flatMap((e) => e.displays.filter((d) => d.output === outputId).map((d) => `${e.name} · Display ${d.index}`));

  let right: React.ReactNode;
  if (selectedEndpoint) {
    right = (
      <EndpointPanel
        key={selectedEndpoint.id}
        layout={layout}
        endpoint={selectedEndpoint}
        onChanged={() => (loadEndpoints(), reload())}
        onError={onError}
        onClose={() => setEndpointId(null)}
      />
    );
  } else if (wallEdit) {
    const wall = wallEdit.id ? layout.walls.find((w) => w.id === wallEdit.id) ?? null : null;
    right = (
      <WallPanel
        key={wallEdit.id ?? "new"}
        layout={layout}
        wall={wall}
        onChanged={reload}
        onError={onError}
        onEditSources={(id) => (setWallEdit(null), selectSource(id))}
        onClose={() => setWallEdit(null)}
        onApply={(w) =>
          (wall ? api.putWall(wall.id, w) : api.createWall(w))
            .then((l) => (setLayout(l), setWallEdit(null)))
            .catch(onError)
        }
        onRemove={(removeOutputs) =>
          wall &&
          api
            .deleteWall(wall.id, removeOutputs)
            .then(() => (reload(), setWallEdit(null)))
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
        onReorder={(to) => reorderSource(selectedSource.id, to)}
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
        shownOn={shownOn(selected.id)}
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
          onWall={(id) => (setMode("outputs"), setWallEdit({ id }))}
          onToggle={(id, enabled) => patch(id, { enabled })}
        />
        <SourcesPanel
          layout={layout}
          selectedId={mode === "sources" ? sourceId : null}
          onSelect={selectSource}
          onReorder={reorderSource}
          onChanged={reload}
          onError={onError}
        />
        <EndpointList
          info={endpoints}
          selectedId={endpointId}
          onSelect={(id) => (setEndpointId(id), setWallEdit(null))}
          onSetUp={setUpEndpoint}
        />
        <ScenesPanel layout={layout} onChanged={reload} onError={onError} />
        <div className="snap-opts">
          <label className="toggle" title="Line frames up with the edges and centres of other frames and of the canvas (hold Alt to drag freely)">
            <input type="checkbox" checked={smartSnap} onChange={(e) => toggleSmartSnap(e.target.checked)} />
            Snap to other frames &amp; centres
          </label>
          <label className="field inline snap">
            <span>Grid</span>
            <select value={snap} onChange={(e) => setSnap(+e.target.value)}>
              {[1, 10, 60, 120, 240].map((s) => (
                <option key={s} value={s}>
                  {s === 1 ? "off (1 px)" : `${s} px`}
                </option>
              ))}
            </select>
          </label>
        </div>
      </aside>
      <main className="main">
        <div className="canvas-info">
          Canvas {layout.canvas.width} × {layout.canvas.height} ·{" "}
          {mode === "outputs" ? "editing what each display shows" : "editing where each source sits"} · drag to move, drag handles to resize, arrow keys nudge
          {mode === "outputs" ? ", round handle or [ ] tilts, a group's tab moves the whole group" : ""}
        </div>
        <CanvasEditor
          layout={layout}
          mode={mode}
          selectedId={mode === "outputs" ? selectedId : sourceId}
          snap={snap}
          smart={smartSnap}
          onSelect={mode === "outputs" ? selectOutput : selectSource}
          onRectChange={onCanvasRect}
          onAngleChange={onAngleChange}
          onGroupMove={onGroupMove}
        />
      </main>
      <aside className="right">{right}</aside>
      <MetricsPanel metrics={metrics} />
    </div>
  );
}
