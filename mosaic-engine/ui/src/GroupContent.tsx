import { useEffect, useState } from "react";
import { dialog, toast } from "./Dialogs";
import { api, wallBounds, type Layout, type Wall } from "./api";
import { FADES } from "./Panels";
import { MediaSelect } from "./Settings";

// What a wall group shows: one NDI stream filling it, or a saved preset (any arrangement
// of sources inside the group). Control systems use the same calls (docs/CONTROL_API.md).
export function GroupContent({
  layout,
  wall,
  onChanged,
  onError,
  onEditSources,
}: {
  layout: Layout;
  wall: Wall;
  onChanged: () => void;
  onError: (e: unknown) => void;
  onEditSources: (sourceId: string) => void;
}) {
  const [streams, setStreams] = useState<string[]>([]);
  const [ndi, setNdi] = useState(true);
  const [fade, setFade] = useState(500);
  useEffect(() => {
    const load = () =>
      api
        .sources()
        .then((r) => (setStreams(r.ndi.discovered), setNdi(r.ndi.available)))
        .catch(() => {});
    load();
    const t = window.setInterval(load, 3000);
    return () => window.clearInterval(t);
  }, []);

  const run = (p: Promise<unknown>) => p.then(onChanged).catch(onError);
  const showing = layout.sources.filter((s) => s.group === wall.id);
  const area = wallBounds(wall);
  // Adds a layer to the group without replacing what it shows (to build a preset).
  const addLayer = (ndiName: string) =>
    api
      .addSource({ name: ndiName, type: "ndi", ndiName, group: wall.id, rect: area })
      .then((s) => (onChanged(), onEditSources(s.id)))
      .catch(onError);

  return (
    <>
      <div className="group">
        <h3>Showing now</h3>
        {showing.length === 0 ? (
          <small className="muted wide-note">Nothing of its own: the group shows the canvas under it.</small>
        ) : (
          <div className="wide-note showing">
            {[...showing].reverse().map((s) => (
              <button key={s.id} className="link" onClick={() => onEditSources(s.id)} title="Edit this layer">
                {s.name}
                {s.type === "ndi" ? "" : ` (${s.type === "solid" ? "colour" : "pattern"})`}
              </button>
            ))}
          </div>
        )}
        <label className="field wide">
          <span>Show an NDI stream on the whole group</span>
          <select value="" disabled={!ndi} onChange={(e) => e.target.value && run(api.showOnGroup(wall.id, e.target.value, fade))}>
            <option value="">{ndi ? `Pick a stream (${streams.length} found)…` : "NDI unavailable in this build"}</option>
            {streams.map((n) => (
              <option key={n} value={n}>
                {n}
              </option>
            ))}
          </select>
        </label>
        <label className="field wide">
          <span>Add a stream as another layer (then place it in Edit sources)</span>
          <select value="" disabled={!ndi} onChange={(e) => e.target.value && addLayer(e.target.value)}>
            <option value="">Pick a stream…</option>
            {streams.map((n) => (
              <option key={n} value={n}>
                {n}
              </option>
            ))}
          </select>
        </label>
        <div className="row buttons">
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
          <button disabled={!showing.length} onClick={() => run(api.clearGroup(wall.id, fade))}>
            Clear group
          </button>
        </div>
      </div>

      <div className="group">
        <label className="field wide">
          <span>If a stream drops, show</span>
          <MediaSelect
            value={wall.fallbackImage ?? ""}
            placeholder={layout.fallbackImage ? `The layout's default (${layout.fallbackImage})` : "The last frame (no image)"}
            onError={onError}
            onChange={(fallbackImage) => run(api.patchWall(wall.id, { fallbackImage }))}
          />
        </label>
      </div>

      <div className="group">
        <h3>Presets</h3>
        {wall.presets.length === 0 && (
          <small className="muted wide-note">Set up what the group should show, then save it as a preset to switch back to it with one click or one API call.</small>
        )}
        <div className="wide-note">
          {wall.presets.map((p) => (
            <div key={p.id} className={`list-item${p.id === wall.activePreset ? " selected" : ""}`}>
              <div className="list-text">
                <div>{p.name}</div>
                <small>
                  id <code>{p.id}</code> · {p.sources.map((s) => s.name).join(", ") || "empty"}
                </small>
              </div>
              <button title="Show this preset" onClick={() => run(api.recallGroupPreset(wall.id, p.id, fade))}>
                ▶
              </button>
              <button title="Overwrite with what the group shows now" onClick={async () => (await dialog.confirm(`Overwrite "${p.name}"?`, { message: `It will hold what ${wall.name} shows now.`, okLabel: "Overwrite" })) && run(api.saveGroupPreset(wall.id, p.name).then(() => toast(`"${p.name}" updated`)))}>
                ⟳
              </button>
              <button title="Delete" onClick={async () => (await dialog.confirm(`Delete preset "${p.name}"?`, { message: "Control systems that recall it will get an error.", okLabel: "Delete", danger: true })) && run(api.deleteGroupPreset(wall.id, p.id))}>
                ✕
              </button>
            </div>
          ))}
        </div>
        <button
          className="wide-note"
          onClick={async () => {
            const name = await dialog.prompt("Save preset", { message: `Saves what ${wall.name} shows now. The same name overwrites a preset.`, value: `Preset ${wall.presets.length + 1}` });
            if (name) run(api.saveGroupPreset(wall.id, name).then((p) => toast(`Preset saved · id ${p.id}`)));
          }}
        >
          + Save current as preset
        </button>
      </div>
    </>
  );
}
