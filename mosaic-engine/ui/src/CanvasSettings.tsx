import { useEffect, useState } from "react";
import { dialog } from "./Dialogs";
import { api, type GpuInfo, type Layout } from "./api";
import { TextField } from "./Inspector";

// Settings → Canvas: this canvas's name, the GPU it renders on (with the monitors on each GPU, for
// display outputs) and its NDI stream names.
export function CanvasSettings({ layout, onChanged, onError }: { layout: Layout; onChanged: () => void; onError: (e: unknown) => void }) {
  const [gpus, setGpus] = useState<GpuInfo[]>([]);
  useEffect(() => {
    api.gpus().then(setGpus).catch(() => {});
  }, [layout.gpu]);
  const put = (p: Partial<Layout>) => api.putLayout(p).then(onChanged).catch(onError);
  const chosen = layout.gpu ?? -1;

  const pickGpu = async (index: number) => {
    const g = gpus.find((x) => x.index === index);
    const ok = await dialog.confirm(index < 0 ? "Render this canvas on the default GPU?" : `Render this canvas on ${g?.name ?? `GPU ${index + 1}`}?`, {
      message: "The canvas starts again on that GPU: its NDI streams and display outputs pause for a moment.",
      okLabel: "Move it",
    });
    if (ok) put({ gpu: index, gpuName: g?.name ?? "" });
  };

  return (
    <section className="list">
      <div className="list-head">
        <h3>Canvas</h3>
      </div>
      <TextField label="Name" value={layout.name} onCommit={(name) => put({ name })} />
      <label className="field wide">
        <span>GPU</span>
        <select value={chosen} onChange={(e) => pickGpu(+e.target.value)}>
          <option value={-1}>Default GPU</option>
          {gpus.map((g) => (
            <option key={g.index} value={g.index}>
              {g.index + 1}: {g.name}
              {g.outputs.length ? ` · ${g.outputs.length} monitor(s)` : ""}
            </option>
          ))}
          {chosen >= 0 && !gpus.some((g) => g.index === chosen) && <option value={chosen}>{layout.gpuName || `GPU ${chosen + 1}`} (not found)</option>}
        </select>
      </label>
      {gpus.map((g) =>
        g.outputs.length ? (
          <small key={g.index} className="muted">
            {g.index + 1}: {g.name} drives {g.outputs.map((o) => `${o.name.replace("\\\\.\\", "")} ${o.width}×${o.height}${o.refreshHz ? ` ${o.refreshHz} Hz` : ""}`).join(", ")}
            {g.canvases.length ? ` · canvases: ${g.canvases.join(", ")}` : ""}
          </small>
        ) : null,
      )}
      <TextField
        label="NDI names"
        value={layout.ndiPrefix ?? "Mosaic_Output"}
        onCommit={(ndiPrefix) => put({ ndiPrefix })}
      />
      <small className="muted">New outputs on this canvas send {(layout.ndiPrefix ?? "Mosaic_Output") + "_01"}, _02, …; existing ones keep their names.</small>
    </section>
  );
}
