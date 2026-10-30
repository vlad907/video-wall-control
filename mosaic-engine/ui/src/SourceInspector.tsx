import type { Layout, Rect, Source } from "./api";
import { NumberField, TextField } from "./Inspector";

// Edits one source: where it sits on the canvas, what part of it is shown, and how opaque it is.
export function SourceInspector({
  layout,
  source,
  onPatch,
  onMove,
  onDelete,
}: {
  layout: Layout;
  source: Source;
  onPatch: (patch: Partial<Source>) => void;
  onMove: (delta: -1 | 1) => void;
  onDelete: () => void;
}) {
  const { width: cw, height: ch } = layout.canvas;
  const rect = (k: keyof Rect) => (v: number) => onPatch({ rect: { ...source.rect, [k]: v } });
  const crop = (k: keyof Rect) => (v: number) => onPatch({ crop: { ...source.crop, [k]: v } });
  const index = layout.sources.findIndex((s) => s.id === source.id);
  const placements: [string, Rect][] = [
    ["Full", { x: 0, y: 0, w: cw, h: ch }],
    ["Left ½", { x: 0, y: 0, w: Math.round(cw / 2), h: ch }],
    ["Right ½", { x: Math.round(cw / 2), y: 0, w: cw - Math.round(cw / 2), h: ch }],
    ["Top ½", { x: 0, y: 0, w: cw, h: Math.round(ch / 2) }],
    ["Bottom ½", { x: 0, y: Math.round(ch / 2), w: cw, h: ch - Math.round(ch / 2) }],
    ["Centre ½", { x: Math.round(cw / 4), y: Math.round(ch / 4), w: Math.round(cw / 2), h: Math.round(ch / 2) }],
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
        {source.type === "ndi" ? source.ndiName : source.type === "solid" ? "solid colour" : "test pattern"}
        {source.status ? ` · ${source.status.status}` : ""}
      </small>

      <div className="group">
        <TextField label="Name" value={source.name} onCommit={(name) => onPatch({ name })} />
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
        <h3>Position on canvas</h3>
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
        <h3>Stacking</h3>
        <div className="row buttons">
          <button disabled={index >= layout.sources.length - 1} onClick={() => onMove(1)}>
            Bring forward
          </button>
          <button disabled={index <= 0} onClick={() => onMove(-1)}>
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
