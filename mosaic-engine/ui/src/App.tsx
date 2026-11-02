import { useCallback, useEffect, useRef, useState } from "react";
import { api, LockedError, type EndpointsInfo, type Layout, type Metrics, type Output, type Rect, type Screen, type Source } from "./api";
import { CanvasEditor, type EditMode } from "./CanvasEditor";
import { EndpointList, EndpointPanel } from "./Endpoints";
import { Inspector } from "./Inspector";
import { Icon } from "./Icons";
import { MonitorView, PresetBar, StatusBar } from "./Monitor";
import { LayoutBar, OutputList, ScenesPanel, SourcesPanel } from "./Panels";
import { SettingsPanel, UnlockDialog } from "./Settings";
import { SourceInspector } from "./SourceInspector";
import { WallPanel } from "./WallPanel";

type Section = "screens" | "inputs" | "presets" | "endpoints" | "monitor" | "settings";
const SECTIONS: [Section, string, string][] = [
  ["screens", "Screens", "Outputs and wall groups: what each display shows"],
  ["inputs", "Inputs", "Sources and layers: where each input sits on the canvas"],
  ["presets", "Presets", "Group presets and scenes"],
  ["endpoints", "Endpoints", "Receiving PCs and their displays"],
  ["monitor", "Monitor", "Engine and output health"],
  ["settings", "Settings", "Layout history and files, media, signal loss, access"],
];

export function App() {
  const [layout, setLayout] = useState<Layout | null>(null);
  const [selectedId, setSelectedId] = useState<string | null>(null); // output id
  const [sourceId, setSourceId] = useState<string | null>(null);
  // Left rail sections. Inputs edits where sources sit; the others edit outputs on the canvas.
  const [section, setSection] = useState<Section>("screens");
  const mode: EditMode = section === "inputs" ? "sources" : "outputs";
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
  // A refused change opens the PIN prompt instead of an error banner.
  const onError = useCallback((e: unknown) => (e instanceof LockedError ? undefined : setError(e instanceof Error ? e.message : String(e))), []);
  const [unlock, setUnlock] = useState(false);
  useEffect(() => {
    const open = () => setUnlock(true);
    window.addEventListener("mosaic-locked", open);
    return () => window.removeEventListener("mosaic-locked", open);
  }, []);

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
    if (id) (setSection((s) => (s === "inputs" || s === "monitor" ? "screens" : s)), setWallEdit(null), setEndpointId(null));
  }, []);
  const selectSource = useCallback((id: string | null) => {
    setSourceId(id);
    if (id) (setSection("inputs"), setWallEdit(null), setEndpointId(null));
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
    const hints: Record<Section, [string, string]> = {
      screens: ["Nothing selected", "Click a display on the canvas or in the list to edit it, or a group's name to set up its displays and content."],
      inputs: ["Nothing selected", "Click a layer on the canvas or in the list to place it, crop it or change its opacity."],
      presets: ["Presets", "Click ▶ to show a preset on its group, or open a group to save new ones. The strip under the canvas does the same in one click."],
      endpoints: ["Endpoints", "Click a receiving PC to choose which output goes on each of its screens."],
      monitor: ["Monitor", "Live numbers from the engine. Amber means an output is falling behind or dropping frames."],
      settings: ["Settings", "Export or restore this layout, upload images, choose what shows when a stream drops, and set a PIN."],
    };
    const [title, text] = hints[section];
    right = (
      <div className="empty-state">
        <h2>{title}</h2>
        <p>{text}</p>
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

  const panel = (() => {
    switch (section) {
      case "screens":
        return (
          <OutputList
            layout={layout}
            selectedId={selectedId}
            onSelect={selectOutput}
            onAdd={addOutput}
            onWall={(id) => (setEndpointId(null), setWallEdit({ id }))}
            onToggle={(id, enabled) => patch(id, { enabled })}
          />
        );
      case "inputs":
        return <SourcesPanel layout={layout} selectedId={sourceId} onSelect={selectSource} onReorder={reorderSource} onChanged={reload} onError={onError} />;
      case "presets":
        return (
          <>
            <section className="list">
              <div className="list-head">
                <h3>Group presets</h3>
              </div>
              {layout.walls.length === 0 && <small className="muted">Make a wall group first (Screens → + Group).</small>}
              {layout.walls.map((w) => (
                <div key={w.id}>
                  <div className="group-head" onClick={() => (setEndpointId(null), setWallEdit({ id: w.id }))} title="Open this group's content">
                    <span className="grow">
                      <b>{w.name}</b> · {w.presets.length} preset(s)
                    </span>
                    <span>✎</span>
                  </div>
                  {w.presets.map((p) => (
                    <div key={p.id} className={`list-item grouped${p.id === w.activePreset ? " selected" : ""}`}>
                      <div className="list-text">
                        <div>{p.name}</div>
                        <small>{p.sources.map((x) => x.name).join(", ") || "empty"}</small>
                      </div>
                      <button title="Show" onClick={() => api.recallGroupPreset(w.id, p.id, 500).then(reload).catch(onError)}>
                        ▶
                      </button>
                    </div>
                  ))}
                </div>
              ))}
            </section>
            <ScenesPanel layout={layout} onChanged={reload} onError={onError} />
          </>
        );
      case "endpoints":
        return <EndpointList info={endpoints} selectedId={endpointId} onSelect={(id) => (setEndpointId(id), setWallEdit(null))} onSetUp={setUpEndpoint} />;
      case "settings":
        return <SettingsPanel layout={layout} onChanged={reload} onError={onError} />;
      case "monitor":
        return (
          <section className="list">
            <div className="list-head">
              <h3>Outputs</h3>
            </div>
            {(metrics?.outputs ?? []).map((o) => {
              const bad = o.enabled && (o.fps < (metrics?.engine.targetFps ?? 60) * 0.97 || o.droppedNoBuffer + o.droppedQueue > 0);
              return (
                <div key={o.id} className="list-item" onClick={() => selectOutput(o.id)}>
                  <span className={`dot ${!o.enabled ? "off" : bad ? "warn" : "on"}`} />
                  <div className="list-text">
                    <div>{o.name}</div>
                    <small>
                      {o.fps.toFixed(1)} fps · {o.connections >= 0 ? `${o.connections} receiver(s)` : o.sink}
                    </small>
                  </div>
                </div>
              );
            })}
          </section>
        );
    }
  })();

  // The right panel follows the section: an endpoint stays open only under Endpoints, a group under Screens / Presets.
  const openSection = (id: Section) => {
    setSection(id);
    if (id !== "endpoints") setEndpointId(null);
    if (id !== "screens" && id !== "presets") setWallEdit(null);
  };

  const engineStatus = metrics && (
    <button className={`engine-pill${metrics.engine.renderFps < metrics.engine.targetFps * 0.97 ? " warn" : ""}`} onClick={() => openSection("monitor")} title="Engine status">
      <span className="dot on" />
      {metrics.engine.renderFps.toFixed(0)} fps
    </button>
  );

  return (
    <div className="app">
      <LayoutBar layout={layout} status={engineStatus} onChanged={reload} onError={onError} />
      {error && (
        <div className="error" onClick={() => setError(null)}>
          {error} <small>(click to dismiss)</small>
        </div>
      )}
      <nav className="rail">
        {SECTIONS.map(([id, label, title]) => (
          <button key={id} className={section === id ? "on" : ""} onClick={() => openSection(id)} title={title}>
            <Icon name={id} size={20} />
            <span>{label}</span>
          </button>
        ))}
      </nav>
      <aside className="sidebar">{panel}</aside>
      <main className="main">
        {section === "monitor" ? (
          <MonitorView metrics={metrics} />
        ) : (
          <>
            <div className="canvas-bar">
              <span className="canvas-title">
                {mode === "outputs" ? "Screens" : "Inputs"} <small>{layout.canvas.width} × {layout.canvas.height}</small>
              </span>
              <span className="muted canvas-hint">
                {mode === "outputs" ? "Drag a frame to choose what a display shows · a group's tab moves the group · [ ] tilts" : "Drag a layer to place it · arrow keys nudge"}
              </span>
              <span className="spacer" />
              <button className={`icon-toggle${smartSnap ? " on" : ""}`} onClick={() => toggleSmartSnap(!smartSnap)} title="Snap to other frames' edges and centres (hold Alt to drag freely)">
                <Icon name="magnet" size={16} /> Snap
              </button>
              <label className="tb-field compact" title="Grid step">
                <Icon name="grid" size={16} />
                <select value={snap} onChange={(e) => setSnap(+e.target.value)}>
                  {[1, 10, 60, 120, 240].map((g) => (
                    <option key={g} value={g}>
                      {g === 1 ? "No grid" : `${g} px`}
                    </option>
                  ))}
                </select>
              </label>
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
            <PresetBar layout={layout} onChanged={reload} onError={onError} />
          </>
        )}
      </main>
      <aside className="right">{right}</aside>
      <StatusBar metrics={metrics} onOpen={() => openSection("monitor")} />
      {unlock && <UnlockDialog onClose={() => (setUnlock(false), reload())} />}
    </div>
  );
}
