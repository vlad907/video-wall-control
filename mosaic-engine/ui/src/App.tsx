import { useCallback, useEffect, useRef, useState } from "react";
import { DialogHost, dialog, toast } from "./Dialogs";
import { api, LockedError, withCanvas, type EndpointsInfo, type Layout, type Metrics, type Output, type Rect, type Screen, type Source } from "./api";
import { BezelPanel } from "./Bezel";
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
  const [multi, setMulti] = useState<string[]>([]); // outputs selected together (bezel correction, moving several)
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

  // Undo / redo: every layout the engine reports (after any change, from any editor) is a step.
  // Changes within 1.5 s of each other (a drag) count as one step.
  const undoStack = useRef<Layout[]>([]);
  const redoStack = useRef<Layout[]>([]);
  const confirmed = useRef<Layout | null>(null); // the engine's layout as last loaded
  const lastStep = useRef(0);
  const replaying = useRef(false);
  const [, setSteps] = useState(0); // re-render when the stacks change
  const reload = useCallback(
    () =>
      api
        .layout()
        .then((l) => {
          const prev = confirmed.current;
          if (prev && prev.id === l.id && JSON.stringify(prev) !== JSON.stringify(l)) {
            if (replaying.current) replaying.current = false;
            else {
              if (performance.now() - lastStep.current > 1500) undoStack.current = [...undoStack.current.slice(-99), prev];
              lastStep.current = performance.now();
              redoStack.current = [];
            }
            setSteps((n) => n + 1);
          }
          if (prev && prev.id !== l.id) (undoStack.current = []), (redoStack.current = []); // another layout: its own history
          confirmed.current = l;
          setLayout(l);
        })
        .catch((e) => setError(String(e))),
    [],
  );
  // A refused change opens the PIN prompt instead of an error banner.
  const onError = useCallback((e: unknown) => (e instanceof LockedError ? undefined : toast(e instanceof Error ? e.message : String(e), "error")), []);
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
    const es = new EventSource(withCanvas("/api/events"));
    es.onmessage = (ev) => {
      const data = JSON.parse(ev.data) as { metrics: Metrics; layoutVersion: number };
      setMetrics(data.metrics?.engine ? data.metrics : null);  // empty while this canvas is (re)starting
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
    setMulti([]);
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

  // Puts a whole earlier layout back (everything but its id).
  const replay = useCallback(
    (from: React.MutableRefObject<Layout[]>, to: React.MutableRefObject<Layout[]>, label: string) => {
      const target = from.current[from.current.length - 1];
      const now = confirmed.current;
      if (!target || !now) return;
      from.current = from.current.slice(0, -1);
      to.current = [...to.current, now];
      replaying.current = true;
      lastStep.current = 0;
      const { id: _id, ...rest } = target;
      api
        .putLayout(rest)
        .then(() => (toast(label), reload()))
        .catch((e) => ((replaying.current = false), onError(e)));
    },
    [onError, reload],
  );
  const undo = useCallback(() => replay(undoStack, redoStack, "Undone"), [replay]);
  const redo = useCallback(() => replay(redoStack, undoStack, "Redone"), [replay]);

  // Ctrl/Cmd+Z undo, Ctrl/Cmd+Shift+Z or Ctrl+Y redo, Delete removes the selection, Ctrl/Cmd+D duplicates it.
  useEffect(() => {
    const onKey = async (e: KeyboardEvent) => {
      if (["INPUT", "SELECT", "TEXTAREA"].includes((e.target as HTMLElement).tagName) || document.querySelector(".modal") || !layout) return;
      const mod = e.metaKey || e.ctrlKey;
      if (mod && e.key.toLowerCase() === "z") return e.preventDefault(), e.shiftKey ? redo() : undo();
      if (mod && e.key.toLowerCase() === "y") return e.preventDefault(), redo();
      if (e.key === "Escape" && multi.length) return setMulti([]);
      // Ctrl/Cmd+A: every display of the selected one's group (or all displays).
      if (mod && e.key.toLowerCase() === "a" && mode === "outputs") {
        e.preventDefault();
        const g = layout.outputs.find((o) => o.id === selectedId)?.group;
        return setMulti(layout.outputs.filter((o) => o.source === "canvas" && (!g || o.group === g)).map((o) => o.id));
      }
      const out = mode === "outputs" && multi.length < 2 ? layout.outputs.find((o) => o.id === selectedId) : undefined;
      const src = mode === "sources" ? layout.sources.find((x) => x.id === sourceId) : undefined;
      if ((e.key === "Delete" || e.key === "Backspace") && (out || src)) {
        e.preventDefault();
        const name = (out ?? src)!.name;
        if (!(await dialog.confirm(`Remove ${name}?`, { message: out ? "Its NDI stream stops. Ctrl+Z brings it back." : "Ctrl+Z brings it back.", okLabel: "Remove", danger: true }))) return;
        (out ? api.deleteOutput(out.id).then(() => setSelectedId(null)) : api.deleteSource(src!.id).then(() => setSourceId(null))).then(reload).catch(onError);
      }
      if (mod && e.key.toLowerCase() === "d" && (out || src)) {
        e.preventDefault();
        if (out) {
          const { id: _id, ndiName: _n, group: _g, ...copy } = out;
          api
            .addOutput({ ...copy, name: `${out.name} copy`, crop: { ...out.crop, x: out.crop.x + 60, y: out.crop.y + 60 } })
            .then((o) => (toast(`Duplicated as ${o.ndiName}`), selectOutput(o.id), reload()))
            .catch(onError);
        } else if (src) {
          const { id: _id, status: _s, ...copy } = src;
          api
            .addSource({ ...copy, name: `${src.name} copy`, rect: { ...src.rect, x: src.rect.x + 60, y: src.rect.y + 60 } })
            .then((x) => (selectSource(x.id), reload()))
            .catch(onError);
        }
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [layout, mode, selectedId, sourceId, multi, undo, redo, reload, onError, selectOutput, selectSource]);

  // Moves several displays at once; a whole wall group moves as the group (so its settings follow).
  const onMultiMove = useCallback(
    (ids: string[], dx: number, dy: number) => {
      if (!layout) return;
      const g = layout.walls.find((w) => {
        const members = layout.outputs.filter((o) => o.group === w.id).map((o) => o.id);
        return members.length === ids.length && members.every((m) => ids.includes(m));
      });
      (g
        ? api.moveWall(g.id, g.x + dx, g.y + dy)
        : api.putLayout({ outputs: layout.outputs.map((o) => (ids.includes(o.id) ? { ...o, crop: { ...o.crop, x: o.crop.x + dx, y: o.crop.y + dy } } : o)) })
      )
        .then(reload)
        .catch(onError);
    },
    [layout, reload, onError],
  );
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
  const setUpEndpoint = async (host: string, screens: Screen[]) => {
    const pcName = host || (await dialog.prompt("Add an endpoint", { message: "The receiving PC's name, as its dashboard shows it (Endpoint: not set up, PC name …).", placeholder: "WALL-PC-1", okLabel: "Next" }));
    if (!pcName) return;
    const name = await dialog.prompt("Name this endpoint", { message: `How ${pcName} is shown in the editor.`, value: pcName, okLabel: "Add" });
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
  if (mode === "outputs" && multi.length > 1) {
    right = <BezelPanel key={[...multi].sort().join(",")} layout={layout} ids={multi} onClose={() => setMulti([])} onError={onError} />;
  } else if (selectedEndpoint) {
    right = (
      <EndpointPanel
        key={selectedEndpoint.id}
        layout={layout}
        endpoint={selectedEndpoint}
        receiverVersion={endpoints?.receiverVersion}
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
        onDelete={async () => {
          if (!(await dialog.confirm(`Remove ${selectedSource.name}?`, { okLabel: "Remove", danger: true }))) return;
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
        onBezel={
          selected.group && layout.walls.some((w) => w.id === selected.group)
            ? () => setMulti(layout.outputs.filter((o) => o.group === selected.group).map((o) => o.id))
            : undefined
        }
        onPatch={(p) => patch(selected.id, p)}
        onDelete={async () => {
          if (!(await dialog.confirm(`Remove ${selected.name}?`, { message: "Its NDI stream stops.", okLabel: "Remove", danger: true }))) return;
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
              const isDisplay = o.sink === "display";
              const bad = o.enabled && (isDisplay ? !o.open || !!o.error || !!o.modeError : o.fps < (metrics?.engine.targetFps ?? 60) * 0.97 || o.droppedNoBuffer + o.droppedQueue > 0);
              return (
                <div key={o.id} className="list-item" onClick={() => selectOutput(o.id)}>
                  <span className={`dot ${!o.enabled ? "off" : bad ? "warn" : "on"}`} />
                  <div className="list-text">
                    <div>{o.name}</div>
                    <small>
                      {isDisplay
                        ? o.error || o.modeError
                          ? `monitor ${o.display ?? ""}: ${o.error || o.modeError}`
                          : `${(o.fps ?? 0).toFixed(1)} fps on ${o.display ?? "a monitor"}${o.heldFrames ? ` · ${o.heldFrames} frames held` : ""}`
                        : `${o.fps.toFixed(1)} fps · ${o.connections >= 0 ? `${o.connections} receiver(s)` : o.sink}`}
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
                {mode === "outputs"
                  ? multi.length > 1
                    ? `${multi.length} displays selected · drag one to move them all · Esc to deselect`
                    : "Drag a frame to choose what a display shows · Shift-click or Shift-drag selects several · [ ] tilts"
                  : "Drag a layer to place it · arrow keys nudge"}
              </span>
              <span className="spacer" />
              <button className="icon-toggle" disabled={!undoStack.current.length} onClick={undo} title="Undo (Ctrl+Z)">
                ↶ Undo
              </button>
              <button className="icon-toggle" disabled={!redoStack.current.length} onClick={redo} title="Redo (Ctrl+Shift+Z)">
                ↷ Redo
              </button>
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
              multi={mode === "outputs" ? multi : []}
              onMulti={setMulti}
              onMultiMove={onMultiMove}
            />
            <PresetBar layout={layout} onChanged={reload} onError={onError} />
          </>
        )}
      </main>
      <aside className="right">{right}</aside>
      <StatusBar metrics={metrics} onOpen={() => openSection("monitor")} />
      {unlock && <UnlockDialog onClose={() => (setUnlock(false), reload())} />}
      <DialogHost />
    </div>
  );
}
