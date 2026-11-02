import { wallBounds, type Layout, type Rect, type Source } from "./api";
import { NumberField, TextField } from "./Inspector";
import { MediaSelect } from "./Settings";

// Edits one source: where it sits on the canvas, what part of it is shown, and how opaque it is.
export function SourceInspector({
  layout,
  source,
  onPatch,
  onReorder,
  onDelete,
}: {
  layout: Layout;
  source: Source;
  onPatch: (patch: Partial<Source>) => void;
  onReorder: (toIndex: number) => void; // 0 = bottom layer
  onDelete: () => void;
}) {
  const { width: cw, height: ch } = layout.canvas;
  const rect = (k: keyof Rect) => (v: number) => onPatch({ rect: { ...source.rect, [k]: v } });
  const crop = (k: keyof Rect) => (v: number) => onPatch({ crop: { ...source.crop, [k]: v } });
  const index = layout.sources.findIndex((s) => s.id === source.id);
  // Placements are within the source's wall group when it has one, else the whole canvas.
  const wall = layout.walls.find((w) => w.id === source.group);
  const area = wall ? wallBounds(wall) : { x: 0, y: 0, w: cw, h: ch };
  const at = (fx: number, fy: number, fw: number, fh: number): Rect => {
    const x = Math.round(area.x + area.w * fx);
    const y = Math.round(area.y + area.h * fy);
    return { x, y, w: Math.round(area.x + area.w * (fx + fw)) - x, h: Math.round(area.y + area.h * (fy + fh)) - y };
  };
  const placements: [string, Rect][] = [
    ["Full", at(0, 0, 1, 1)],
    ["Left ½", at(0, 0, 0.5, 1)],
    ["Right ½", at(0.5, 0, 0.5, 1)],
    ["Top ½", at(0, 0, 1, 0.5)],
    ["Bottom ½", at(0, 0.5, 1, 0.5)],
    ["Top-left ¼", at(0, 0, 0.5, 0.5)],
    ["Top-right ¼", at(0.5, 0, 0.5, 0.5)],
    ["Bottom-left ¼", at(0, 0.5, 0.5, 0.5)],
    ["Bottom-right ¼", at(0.5, 0.5, 0.5, 0.5)],
    ["Centre ½", at(0.25, 0.25, 0.5, 0.5)],
  ];
  const cropped = source.crop.w > 0 && source.crop.h > 0;
  const natW = source.status?.width ?? 0;
  const natH = source.status?.height ?? 0;

  return (
    <div className="inspector">
      <div className="inspector-head">
        <h2>{source.name}</h2>
        <label className="toggle">
          <input type="checkbox" checked={source.enabled} onChange={(e) => onPatch({ enabled: e.target.checked })} />
          enabled
        </label>
      </div>
      <small className="muted">
        {source.type === "ndi" ? source.ndiName : source.type === "solid" ? "solid colour" : source.type === "image" ? "image" : "test pattern"}
        {source.status ? ` · ${source.status.status}` : ""}
      </small>

      <div className="group">
        <TextField label="Name" value={source.name} onCommit={(name) => onPatch({ name })} />
        {source.type === "image" && (
          <label className="field wide">
            <span>Image</span>
            <MediaSelect value={source.image ?? ""} placeholder="Choose an image…" onError={() => {}} onChange={(image) => onPatch({ image })} />
          </label>
        )}
      </div>

      <div className="group">
        <h3>Opacity</h3>
        <div className="slider wide-note">
          <input
            type="range"
            min={0}
            max={100}
            value={Math.round(source.opacity * 100)}
            onChange={(e) => onPatch({ opacity: +e.target.value / 100 })}
          />
          <span>{Math.round(source.opacity * 100)}%</span>
        </div>
      </div>

      <div className="group">
        <label className="field wide">
          <span>Belongs to group (saved in that group's presets)</span>
          <select
            value={source.group ?? ""}
            onChange={(e) => {
              const g = layout.walls.find((w) => w.id === e.target.value);
              // Moving into a group starts by filling it.
              onPatch(g ? { group: g.id, rect: wallBounds(g) } : { group: "" });
            }}
          >
            <option value="">None (whole canvas)</option>
            {layout.walls.map((w) => (
              <option key={w.id} value={w.id}>
                {w.name}
              </option>
            ))}
          </select>
        </label>
      </div>

      <div className="group">
        <h3>Position {wall ? `in ${wall.name}` : "on canvas"}</h3>
        <div className="row">
          <NumberField label="X" value={source.rect.x} min={-100000} onCommit={rect("x")} />
          <NumberField label="Y" value={source.rect.y} min={-100000} onCommit={rect("y")} />
        </div>
        <div className="row">
          <NumberField label="W" value={source.rect.w} min={1} onCommit={rect("w")} />
          <NumberField label="H" value={source.rect.h} min={1} onCommit={rect("h")} />
        </div>
        <div className="chips wide-note">
          {placements.map(([label, r]) => (
            <button key={label} onClick={() => onPatch({ rect: r })}>
              {label}
            </button>
          ))}
        </div>
      </div>

      <div className="group">
        <h3>Source crop</h3>
        {cropped ? (
          <>
            <div className="row">
              <NumberField label="X" value={source.crop.x} onCommit={crop("x")} />
              <NumberField label="Y" value={source.crop.y} onCommit={crop("y")} />
            </div>
            <div className="row">
              <NumberField label="W" value={source.crop.w} min={1} onCommit={crop("w")} />
              <NumberField label="H" value={source.crop.h} min={1} onCommit={crop("h")} />
            </div>
            <button className="wide-note" onClick={() => onPatch({ crop: { x: 0, y: 0, w: 0, h: 0 } })}>
              Show the whole source
            </button>
          </>
        ) : (
          <button
            className="wide-note"
            onClick={() => onPatch({ crop: { x: 0, y: 0, w: natW || source.rect.w, h: natH || source.rect.h } })}
          >
            Crop the source…
          </button>
        )}
      </div>

      <div className="group">
        <h3>
          Layer {layout.sources.length - index} of {layout.sources.length}
          {index === layout.sources.length - 1 && layout.sources.length > 1 ? " (top)" : ""}
        </h3>
        <div className="row buttons">
          <button disabled={index >= layout.sources.length - 1} onClick={() => onReorder(layout.sources.length - 1)}>
            Bring to front
          </button>
          <button disabled={index <= 0} onClick={() => onReorder(0)}>
            Send to back
          </button>
        </div>
        <div className="row buttons">
          <button disabled={index >= layout.sources.length - 1} onClick={() => onReorder(index + 1)}>
            Bring forward
          </button>
          <button disabled={index <= 0} onClick={() => onReorder(index - 1)}>
            Send backward
          </button>
        </div>
      </div>

      <button className="danger" onClick={onDelete}>
        Remove source
      </button>
    </div>
  );
}
