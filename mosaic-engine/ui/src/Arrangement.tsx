import { useEffect, useRef, useState } from "react";
import { dialog, toast } from "./Dialogs";
import { api, type ControlInfo, type Playlist, type SlotContent, type Wall } from "./api";

// The control summary (what control systems read), refreshed every few seconds.
export function useControl(): [ControlInfo | null, () => void] {
  const [info, setInfo] = useState<ControlInfo | null>(null);
  const load = () => {
    api.control().then(setInfo).catch(() => {});
  };
  useEffect(() => {
    load();
    const t = window.setInterval(load, 3000);
    return () => window.clearInterval(t);
  }, []);
  return [info, load];
}

const valueOf = (c: SlotContent) => (c.ndiName ? `ndi:${c.ndiName}` : c.playlist ? `playlist:${c.playlist}` : c.video ? `video:${c.video}` : "");
const contentOf = (v: string): SlotContent =>
  v.startsWith("ndi:") ? { ndiName: v.slice(4) } : v.startsWith("playlist:") ? { playlist: v.slice(9) } : v.startsWith("video:") ? { video: v.slice(6) } : {};

// Picks what a slot or the background plays: an NDI stream or a playlist.
export function ContentSelect({ value, info, empty, onChange }: { value: SlotContent; info: ControlInfo; empty: string; onChange: (c: SlotContent) => void }) {
  const v = valueOf(value);
  const known = info.ndi.streams.some((n) => `ndi:${n}` === v) || info.playlists.some((p) => `playlist:${p.id}` === v);
  return (
    <select value={v} onChange={(e) => onChange(contentOf(e.target.value))}>
      <option value="">{empty}</option>
      {v && !known && <option value={v}>{value.ndiName ? `${value.ndiName} (not on the network)` : value.playlist ? `Playlist: ${value.playlist} (missing)` : `Video: ${value.video}`}</option>}
      <optgroup label="NDI streams">
        {info.ndi.streams.map((n) => (
          <option key={n} value={`ndi:${n}`}>
            {n}
          </option>
        ))}
      </optgroup>
      {info.playlists.length > 0 && (
        <optgroup label="Playlists">
          {info.playlists.map((p) => (
            <option key={p.id} value={`playlist:${p.id}`}>
              Playlist: {p.name} ({p.videos} video{p.videos === 1 ? "" : "s"})
            </option>
          ))}
        </optgroup>
      )}
    </select>
  );
}

// How a group's area is divided, and the stream in each part. The same calls a control system
// makes (docs/CONTROL_API.md, "Arrangements").
export function GroupArrangement({ wall, fade, onChanged, onError }: { wall: Wall; fade: number; onChanged: () => void; onError: (e: unknown) => void }) {
  const [info, reload] = useControl();
  const g = info?.groups.find((x) => x.id === wall.id);
  if (!info || !g) return null;
  const run = (p: Promise<unknown>) => p.then(() => (reload(), onChanged())).catch(onError);
  return (
    <div className="group">
      <h3>Arrangement</h3>
      <label className="toggle wide-note" title="Off: the group shows what lies under it on the canvas (the rest of the mosaic, the background). On: its own streams come back.">
        <input type="checkbox" checked={g.on} disabled={!g.on && !g.canRestore} onChange={(e) => run(api.setGroupContentOn(wall.id, e.target.checked, fade))} />
        {g.on ? "Shows its own content" : g.canRestore ? "Off: follows what's under it (tick to bring its content back)" : "Nothing of its own yet: pick an arrangement"}
      </label>
      <label className="field wide">
        <span>Divide the group as</span>
        <select value={g.arrangement} onChange={(e) => e.target.value && run(api.arrangeGroup(wall.id, e.target.value, fade))}>
          {!g.arrangement && <option value="">{g.on ? "Placed by hand (Edit sources)" : "Pick one…"}</option>}
          {g.arrangements.map((a) => (
            <option key={a.id} value={a.id}>
              {a.name}
            </option>
          ))}
        </select>
      </label>
      {g.slots.map((s) => (
        <label key={s.slot} className="field wide">
          <span>
            {s.slot}. {s.name}
            {s.signal === "lost" ? " · no signal" : ""}
          </span>
          <ContentSelect value={s} info={info} empty="Nothing" onChange={(c) => run(api.setGroupSlot(wall.id, s.slot, c, fade))} />
        </label>
      ))}
    </div>
  );
}

// Settings: the background (one stream under the whole canvas) and the playlists.
export function BackgroundSettings({ onChanged, onError }: { onChanged: () => void; onError: (e: unknown) => void }) {
  const [info, reload] = useControl();
  const [lists, setLists] = useState<Playlist[]>([]);
  const [folder, setFolder] = useState("");
  const [busy, setBusy] = useState("");
  const picker = useRef<HTMLInputElement>(null);
  const target = useRef("");
  const loadLists = () => {
    api
      .playlists()
      .then((r) => (setLists(r.playlists), setFolder(r.folder)))
      .catch(() => {});
  };
  useEffect(loadLists, []);
  if (!info) return null;
  const bg = info.background;
  const run = (p: Promise<unknown>) => p.then(() => (reload(), loadLists(), onChanged())).catch(onError);
  const upload = async (files: FileList | null) => {
    for (const f of Array.from(files ?? [])) {
      try {
        await api.uploadMedia(f, (fraction) => setBusy(`${f.name} ${Math.round(fraction * 100)}%`), target.current);
      } catch (e) {
        onError(e);
      }
    }
    setBusy("");
    reload();
    loadLists();
  };
  return (
    <section className="list">
      <div className="list-head">
        <h3>Background</h3>
      </div>
      <small className="muted">One stream or playlist under the whole canvas. Groups that are switched off, and gaps between groups, show it.</small>
      <label className="toggle">
        <input type="checkbox" checked={bg.on} disabled={!bg.chosen} onChange={(e) => run(api.setBackground({ on: e.target.checked, transitionMs: 500 }))} />
        Background on
      </label>
      <label className="field wide">
        <span>Plays</span>
        <ContentSelect value={bg} info={info} empty="Pick a stream or playlist…" onChange={(c) => (c.ndiName || c.playlist || c.video) && run(api.setBackground({ ...c, on: true, transitionMs: 500 }))} />
      </label>

      <div className="list-head">
        <h3>Playlists</h3>
        <button
          onClick={async () => {
            const name = await dialog.prompt("New playlist", { message: "A folder of videos that play one after another and start over.", value: "" });
            if (name) run(api.createPlaylist(name).then(() => toast(`Playlist "${name}" made`)));
          }}
        >
          + Playlist
        </button>
      </div>
      <input ref={picker} type="file" accept=".mp4,.mov,.m4v,video/*" multiple hidden onChange={(e) => (upload(e.target.files), (e.target.value = ""))} />
      {lists.length === 0 && <small className="muted">None yet. A playlist plays its videos in name order (01-intro.mp4, 02-…), and picks up videos added later.</small>}
      {lists.map((p) => (
        <div key={p.id} className="list-item">
          <div className="list-text">
            <div>{p.name}</div>
            <small>{(p.files ?? []).map((f) => f.name).join(", ") || "no videos yet"}</small>
          </div>
          <button title="Add videos (.mp4 .mov .m4v)" disabled={!!busy} onClick={() => ((target.current = p.id), picker.current?.click())}>
            + Videos
          </button>
          <button
            title="Delete the playlist and its videos"
            onClick={async () =>
              (await dialog.confirm(`Delete playlist "${p.name}"?`, { message: `Its ${p.videos} video(s) are deleted from the media server.`, okLabel: "Delete", danger: true })) && run(api.deletePlaylist(p.id))
            }
          >
            ✕
          </button>
        </div>
      ))}
      {busy && <small className="muted">Uploading {busy}</small>}
      {folder && <small className="muted">Videos can also be copied straight into a folder inside {folder} on the media server.</small>}
    </section>
  );
}
