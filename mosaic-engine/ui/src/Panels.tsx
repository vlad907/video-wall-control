import { useEffect, useState } from "react";
import { dialog, toast } from "./Dialogs";
import { api, currentCanvas, switchCanvas, type CanvasInfo, type Layout, type LayoutSummary, type Output, type Source } from "./api";
import { COLORS } from "./CanvasEditor";

const fmt = (v: number, d = 1) => (Number.isFinite(v) ? v.toFixed(d) : "–");

// The canvases running on the media server (each a layout on its own render thread): pick the
// one to edit, add one, or stop one.
function CanvasPick({ onError }: { onError: (e: unknown) => void }) {
  const [list, setList] = useState<CanvasInfo[]>([]);
  const [problems, setProblems] = useState<string[]>([]);
  const load = () =>
    api
      .canvases()
      .then((r) => {
        setList(r.canvases);
        setProblems(r.problems);
        // The canvas this browser remembered was stopped (here or elsewhere): go to the primary.
        if (currentCanvas() && !r.canvases.some((c) => c.id === currentCanvas())) switchCanvas("");
      })
      .catch(() => {});
  useEffect(() => {
    load();
    const t = window.setInterval(load, 5000);
    return () => window.clearInterval(t);
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const current = list.find((c) => c.current);
  const add = async () => {
    const name = await dialog.prompt("New canvas", {
      message:
        "A canvas is a layout that runs at the same time as the others, on its own render thread: a separate video wall or group of receiving PCs. It starts with a test pattern; add a wall group to give it displays. Its NDI streams are named after it.",
      value: `Mosaic ${list.length + 1}`,
      okLabel: "Add canvas",
    });
    if (!name) return;
    api
      .addCanvas({ name })
      .then((r) => {
        toast(`Added "${name}"`);
        switchCanvas(r.id);
      })
      .catch(onError);
  };
  const stop = async () => {
    if (!current || current.primary) return;
    const ok = await dialog.confirm(`Stop the canvas "${current.name}"?`, {
      message: "Its NDI streams stop and its receiving PCs go blank. The layout stays saved: add it again as a canvas any time.",
      okLabel: "Stop canvas",
    });
    if (ok) api.stopCanvas(current.id).then(() => switchCanvas("")).catch(onError);
  };
  if (!list.length) return null;
  return (
    <>
      <label className="tb-field layout-pick" title={problems.length ? problems.join("\n") : "The canvas you're editing. Each canvas runs at the same time, with its own displays and receiving PCs."}>
        <span>Canvas{problems.length ? " ⚠" : ""}</span>
        <select value={current?.id ?? ""} onChange={(e) => switchCanvas(list.find((c) => c.id === e.target.value)?.primary ? "" : e.target.value)}>
          {list.map((c) => (
            <option key={c.id} value={c.id}>
              {c.name}
              {c.running ? "" : " (not running)"}
            </option>
          ))}
        </select>
      </label>
      <button className="ghost" title="Add a canvas: another wall or receiver group, running at the same time" onClick={add}>
        + Canvas
      </button>
      {current && !current.primary && (
        <button className="ghost" title="Stop this canvas (its layout stays saved)" onClick={stop}>
          Stop canvas
        </button>
      )}
    </>
  );
}

export function LayoutBar({
  layout,
  status,
  onChanged,
  onError,
}: {
  layout: Layout;
  status: React.ReactNode;
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
    ["4K UHD · 3840 × 2160", 3840, 2160],
    ["8K UHD · 7680 × 4320 (2×2 of 4K)", 7680, 4320],
    ["2 × 4K side by side · 7680 × 2160", 7680, 2160],
    ["4 × 4K in a row · 15360 × 2160", 15360, 2160],
  ];
  const canvasKey = `${layout.canvas.width}x${layout.canvas.height}`;
  // Sources that covered the whole old canvas keep covering it (as the wall tool does).
  const resizeCanvas = (width: number, height: number) => {
    const { width: ow, height: oh } = layout.canvas;
    const sources = layout.sources.map((s) =>
      s.rect.x === 0 && s.rect.y === 0 && s.rect.w === ow && s.rect.h === oh ? { ...s, rect: { x: 0, y: 0, w: width, h: height } } : s,
    );
    run(api.putLayout({ canvas: { ...layout.canvas, width, height }, sources }));
  };

  return (
    <header className="topbar">
      <div className="brand">
        <span className="logo" />
        Mosaic
      </div>
      <CanvasPick onError={onError} />
      <label className="tb-field layout-pick" title="The layout on this canvas: displays, groups, sources and endpoints">
        <span>Layout</span>
        <select value={layout.id} onChange={(e) => run(api.activate(e.target.value))}>
          {layouts.map((l) => (
            <option key={l.id} value={l.id} disabled={l.running && !l.active}>
              {l.name}
              {l.running && !l.active ? " (on another canvas)" : ""}
            </option>
          ))}
        </select>
      </label>
      <button
        className="ghost"
        onClick={async () => {
          const name = await dialog.prompt("Save layout as", { message: "Saves a copy of this layout and switches to it.", value: `${layout.name} copy` });
          if (name) run(api.saveAs(name).then((l) => (toast(`Saved "${name}"`), api.activate(l.id))));
        }}
      >
        Save as…
      </button>
      <button
        className="ghost"
        title="Delete a saved layout (not the active one)"
        onClick={async () => {
          const others = layouts.filter((l) => !l.active && !l.running);
          if (!others.length) return dialog.alert("Nothing to delete", "Layouts on air (on any canvas) can't be deleted, and there are no others.");
          const id = await dialog.prompt("Delete a layout", {
            message: "Layouts on air (on any canvas) can't be deleted. A deleted layout can still be restored from its history (Settings).",
            choices: others.map((l) => ({ value: l.id, label: l.name })),
            okLabel: "Delete",
          });
          if (id) run(api.deleteLayout(id).then(() => toast("Layout deleted")));
        }}
      >
        Delete…
      </button>
      <span className="spacer" />
      <label className="tb-field">
        <span>Canvas</span>
        <select
          value={presets.some(([, w, h]) => `${w}x${h}` === canvasKey) ? canvasKey : "custom"}
          onChange={(e) => {
            const p = presets.find(([, w, h]) => `${w}x${h}` === e.target.value);
            if (p) resizeCanvas(p[1], p[2]);
            else {
              dialog.prompt("Canvas size", { message: "Width × height in pixels, up to 16384 each.", value: `${layout.canvas.width}x${layout.canvas.height}`, okLabel: "Resize" }).then((v) => {
                const m = v?.match(/(\d+)\s*[x×]\s*(\d+)/);
                if (m) resizeCanvas(+m[1], +m[2]);
              });
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
      <label className="tb-field">
        <span>FPS</span>
        <select value={layout.frameRate} onChange={(e) => run(api.putLayout({ frameRate: +e.target.value }))}>
          {[24, 25, 30, 50, 59.94, 60].map((f) => (
            <option key={f} value={f}>
              {f}
            </option>
          ))}
        </select>
      </label>
      <label className="tb-field" title="Receiving PCs show each frame this long after it's made, so network stalls shorter than this don't show. Off for live cameras; about 300 ms for playback (delay the audio to match).">
        <span>Buffer</span>
        <select value={layout.receiverBufferMs ?? 0} onChange={(e) => run(api.putLayout({ receiverBufferMs: +e.target.value }))}>
          {[0, 100, 200, 300, 400, 500].map((ms) => (
            <option key={ms} value={ms}>
              {ms ? `${ms} ms` : "Off"}
            </option>
          ))}
        </select>
      </label>
      {status}
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
  onWall: (id: string | null) => void; // null = new group
  onToggle: (id: string, enabled: boolean) => void;
}) {
  const colour = (id: string) => COLORS[layout.outputs.findIndex((o) => o.id === id) % COLORS.length];
  const row = (o: Output, grouped: boolean) => (
    <div key={o.id} className={`list-item${o.id === selectedId ? " selected" : ""}${grouped ? " grouped" : ""}`} onClick={() => onSelect(o.id)}>
      <span className="swatch" style={{ background: colour(o.id) }} />
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
  );
  const standalone = layout.outputs.filter((o) => !layout.walls.some((w) => w.id === o.group));
  return (
    <section className="list">
      <div className="list-head">
        <h3>Outputs ({layout.outputs.length})</h3>
        <span>
          <button onClick={() => onWall(null)} title="A grid of displays with bezel gaps, or one display (1 × 1)">
            + Group
          </button>{" "}
          <button onClick={onAdd}>+ Output</button>
        </span>
      </div>
      {layout.walls.map((w) => (
        <div key={w.id}>
          <div className="group-head" onClick={() => onWall(w.id)} title="Edit this group">
            <span className="grow">
              <b>{w.name}</b> · {w.cols} × {w.rows} · {w.outputWidth}×{w.outputHeight}
              {w.rotation ? ` ↻${w.rotation}°` : ""}
              {w.gapX || w.gapY ? ` · gaps ${w.gapX}/${w.gapY}` : ""}
            </span>
            <span>✎</span>
          </div>
          {layout.outputs.filter((o) => o.group === w.id).map((o) => row(o, true))}
        </div>
      ))}
      {layout.walls.length > 0 && standalone.length > 0 && <div className="group-head plain">Standalone</div>}
      {standalone.map((o) => row(o, false))}
    </section>
  );
}

export function SourcesPanel({
  layout,
  selectedId,
  onSelect,
  onReorder,
  onChanged,
  onError,
}: {
  layout: Layout;
  selectedId: string | null;
  onSelect: (id: string) => void;
  onReorder: (id: string, toIndex: number) => void; // index in layout.sources (0 = bottom)
  onChanged: () => void;
  onError: (e: unknown) => void;
}) {
  const [info, setInfo] = useState<{ sources: Source[]; ndi: { available: boolean; discovered: string[] } } | null>(null);
  const [dragId, setDragId] = useState<string | null>(null);
  const [drop, setDrop] = useState<{ id: string; above: boolean } | null>(null);
  const refresh = () => api.sources().then(setInfo).catch(onError);
  useEffect(() => {
    refresh();
    const t = window.setInterval(refresh, 3000);
    return () => window.clearInterval(t);
  }, [layout]); // eslint-disable-line react-hooks/exhaustive-deps

  const run = (p: Promise<unknown>) => p.then(() => (refresh(), onChanged())).catch(onError);
  const add = (s: Partial<Source>) => api.addSource({ rect: { x: 0, y: 0, w: layout.canvas.width, h: layout.canvas.height }, ...s }).then((c) => (onSelect(c.id), refresh(), onChanged())).catch(onError);
  const status = (id: string) => info?.sources.find((s) => s.id === id)?.status;
  const n = layout.sources.length;
  // Shown top layer first; layout.sources is bottom first (later ones draw over earlier ones).
  const layers = [...layout.sources].reverse();

  const onDrop = (target: Source) => {
    if (!dragId || !drop || dragId === target.id) return;
    const from = layout.sources.findIndex((s) => s.id === dragId);
    let to = layout.sources.findIndex((s) => s.id === target.id) + (drop.above ? 1 : 0); // "above" = drawn over it
    if (from < to) to -= 1;
    onReorder(dragId, Math.max(0, Math.min(n - 1, to)));
  };

  return (
    <section className="list">
      <div className="list-head">
        <h3 title="The top layer is drawn over the ones below it">Layers (top first)</h3>
        <select
          value=""
          onChange={(e) => {
            if (e.target.value === "pattern") add({ name: "Test pattern", type: "test_pattern" });
            if (e.target.value === "solid") add({ name: "Black", type: "solid", color: [0, 0, 0, 1] });
            if (e.target.value === "image") add({ name: "Image", type: "image", image: "" });
            if (e.target.value === "video") add({ name: "Video", type: "video", video: "", loop: true, playing: true });
            if (e.target.value === "capture")
              api
                .capture()
                .then((c) => {
                  const real = c.devices.filter((d) => d.kind === "device");
                  const dev = (real[0] ?? c.devices[0])?.name ?? "";
                  add({ name: "Capture card", type: "capture", device: dev });
                })
                .catch(onError);
          }}
        >
          <option value="">+ Add…</option>
          <option value="pattern">Test pattern</option>
          <option value="solid">Solid colour</option>
          <option value="image">Image</option>
          <option value="video">Video file</option>
          <option value="capture">Capture card</option>
        </select>
      </div>
      {layers.map((s, i) => {
        const st = status(s.id);
        const index = n - 1 - i;
        return (
          <div
            key={s.id}
            draggable
            onDragStart={(e) => (setDragId(s.id), e.dataTransfer.setData("text/plain", s.id), (e.dataTransfer.effectAllowed = "move"))}
            onDragEnd={() => (setDragId(null), setDrop(null))}
            onDragOver={(e) => {
              if (!dragId) return;
              e.preventDefault();
              const r = e.currentTarget.getBoundingClientRect();
              setDrop({ id: s.id, above: e.clientY < r.top + r.height / 2 });
            }}
            onDrop={(e) => (e.preventDefault(), onDrop(s), setDragId(null), setDrop(null))}
            className={`list-item${s.id === selectedId ? " selected" : ""}${dragId === s.id ? " dragging" : ""}${
              drop?.id === s.id && dragId !== s.id ? (drop.above ? " drop-above" : " drop-below") : ""
            }`}
            onClick={() => onSelect(s.id)}
            title="Drag to change which layer is on top"
          >
            <div className="layer-btns" onClick={(e) => e.stopPropagation()}>
              <button title="Bring forward" disabled={index === n - 1} onClick={() => onReorder(s.id, index + 1)}>
                ▲
              </button>
              <button title="Send backward" disabled={index === 0} onClick={() => onReorder(s.id, index - 1)}>
                ▼
              </button>
            </div>
            <div className="list-text">
              <div>
                {s.name} <small>({s.type === "ndi" ? "NDI" : s.type === "solid" ? "colour" : s.type === "image" ? "image" : s.type === "video" ? "video" : s.type === "capture" ? "capture" : "pattern"})</small>
                {i === 0 && n > 1 && <span className="layer-badge">top</span>}
              </div>
              <small>
                {s.opacity < 1 ? `${Math.round(s.opacity * 100)}% · ` : ""}
                {st?.signal === "lost" ? (st.showing ? "no signal · showing fallback image" : "no signal") : (st?.status ?? "…")}
                {st?.inputFps ? ` · ${fmt(st.inputFps)} fps` : ""}
                {st?.sentFps && st.inputFps && st.sentFps > st.inputFps * 1.15 ? ` of ${fmt(st.sentFps)} sent` : ""}
                {st?.framesDropped ? ` · ${st.framesDropped} dropped here` : ""}
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

export const FADES: [string, number][] = [
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
          onClick={async () => {
            const name = await dialog.prompt("Save scene", { message: "Saves all current sources (the whole canvas).", value: `Scene ${layout.scenes.length + 1}` });
            if (name) run(api.saveScene(name).then(() => toast(`Scene "${name}" saved`)));
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
          <button title="Overwrite with the current sources" onClick={async () => (await dialog.confirm(`Overwrite "${sc.name}"?`, { message: "It will hold the current sources.", okLabel: "Overwrite" })) && run(api.saveScene(sc.name))}>
            ⟳
          </button>
          <button title="Delete" onClick={async () => (await dialog.confirm(`Delete scene "${sc.name}"?`, { okLabel: "Delete", danger: true })) && run(api.deleteScene(sc.id))}>
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
