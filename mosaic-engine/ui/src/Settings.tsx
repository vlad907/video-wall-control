import { useEffect, useRef, useState } from "react";
import { dialog, toast } from "./Dialogs";
import { api, storeKey, storedKey, type Layout, type LayoutVersion, type MediaFile } from "./api";

const kb = (n: number) => (n > 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1024))} KB`);

// Picks an image file from the media folder; offers to upload one too.
export function MediaSelect({ value, onChange, placeholder, onError }: { value: string; onChange: (name: string) => void; placeholder: string; onError: (e: unknown) => void }) {
  const [files, setFiles] = useState<MediaFile[]>([]);
  const input = useRef<HTMLInputElement>(null);
  useEffect(() => {
    api.media().then(setFiles).catch(() => {});
  }, []);
  return (
    <>
      <select
        value={value}
        onChange={(e) => (e.target.value === "\u0000upload" ? input.current?.click() : onChange(e.target.value))}
      >
        <option value="">{placeholder}</option>
        {files.map((f) => (
          <option key={f.name} value={f.name}>
            {f.name}
          </option>
        ))}
        {value && !files.some((f) => f.name === value) && <option value={value}>{value} (missing)</option>}
        <option value={"\u0000upload"}>Upload an image…</option>
      </select>
      <input
        ref={input}
        type="file"
        accept="image/png,image/jpeg,image/bmp,image/gif"
        hidden
        onChange={(e) => {
          const f = e.target.files?.[0];
          e.target.value = "";
          if (f) api.uploadMedia(f).then((m) => (api.media().then(setFiles), onChange(m.name))).catch(onError);
        }}
      />
    </>
  );
}

// Settings section (left panel): layout history and files, media, signal loss, access.
export function SettingsPanel({ layout, onChanged, onError }: { layout: Layout; onChanged: () => void; onError: (e: unknown) => void }) {
  const [history, setHistory] = useState<LayoutVersion[]>([]);
  const [media, setMedia] = useState<MediaFile[]>([]);
  const [security, setSecurity] = useState<{ pinSet: boolean; apiKeySet: boolean } | null>(null);
  const importInput = useRef<HTMLInputElement>(null);
  const uploadInput = useRef<HTMLInputElement>(null);

  const loadHistory = () => api.history(layout.id).then(setHistory).catch(onError);
  const loadMedia = () => api.media().then(setMedia).catch(onError);
  const loadSecurity = () => api.security().then(setSecurity).catch(onError);
  useEffect(() => {
    loadHistory();
  }, [layout]); // eslint-disable-line react-hooks/exhaustive-deps
  useEffect(() => {
    loadMedia();
    loadSecurity();
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const exportLayout = async () => {
    const l = await api.getLayout(layout.id);
    const url = URL.createObjectURL(new Blob([JSON.stringify(l, null, 2)], { type: "application/json" }));
    const a = document.createElement("a");
    a.href = url;
    a.download = `${layout.id}.json`;
    a.click();
    URL.revokeObjectURL(url);
  };

  const importLayout = async (file: File) => {
    try {
      const l = JSON.parse(await file.text()) as Layout;
      if (!l.outputs || !l.canvas) throw new Error("That file isn't a layout export.");
      const existing = await api.layouts();
      if (
        existing.some((x) => x.id === l.id) &&
        !(await dialog.confirm(`Replace layout "${l.name}"?`, { message: "A layout with this id exists. Its current version stays in its history. Cancel to import under a new name.", okLabel: "Replace" }))
      ) {
        const name = await dialog.prompt("Import under a new name", { value: `${l.name} (imported)`, okLabel: "Import" });
        if (!name) return;
        l.id = "";
        l.name = name;
      }
      const saved = await api.importLayout(l);
      toast(`Imported "${saved.name}"`);
      if (await dialog.confirm(`Switch to "${saved.name}" now?`, { okLabel: "Switch" })) await api.activate(saved.id);
      onChanged();
    } catch (e) {
      onError(e);
    }
  };

  const setKey = async (field: "editorPin" | "apiKey", label: string) => {
    const v = await dialog.prompt(`Set the ${label}`, { message: "Leave it empty to remove it.", allowEmpty: true, password: field === "editorPin" });
    if (v === null) return;
    toast(v.trim() ? `${label[0].toUpperCase()}${label.slice(1)} set` : `${label[0].toUpperCase()}${label.slice(1)} removed`);
    api
      .putSecurity({ [field]: v.trim() })
      .then((s) => {
        setSecurity(s);
        // The editor keeps working with the new PIN without asking for it again.
        if (field === "editorPin" && v.trim()) storeKey(v.trim());
        if (field === "editorPin" && !v.trim() && !s.apiKeySet) storeKey("");
      })
      .catch(onError);
  };

  return (
    <>
      <section className="list">
        <div className="list-head">
          <h3>Layout file</h3>
        </div>
        <div className="row-buttons">
          <button onClick={exportLayout} title="Download this layout as a .json file (backup, or to copy it to another server)">
            Export
          </button>
          <button onClick={() => importInput.current?.click()} title="Load a layout file exported before">
            Import…
          </button>
        </div>
        <input
          ref={importInput}
          type="file"
          accept="application/json,.json"
          hidden
          onChange={(e) => {
            const f = e.target.files?.[0];
            e.target.value = "";
            if (f) importLayout(f);
          }}
        />
      </section>

      <section className="list">
        <div className="list-head">
          <h3 title="Saved automatically before changes (at most every 30 s), and before a restore or delete. The last 50 are kept.">History</h3>
          <button onClick={loadHistory}>Refresh</button>
        </div>
        {history.length === 0 && <small className="muted">Earlier versions of this layout appear here as you make changes.</small>}
        {history.map((v) => (
          <div key={v.version} className="list-item">
            <div className="list-text">
              <div>{v.time}</div>
              <small>
                {v.outputs} output(s) · {v.name}
              </small>
            </div>
            <button
              title="Put this version back (what's there now is kept in the history)"
              onClick={async () =>
                (await dialog.confirm(`Restore the layout as it was at ${v.time}?`, { message: "What's there now is kept in the history.", okLabel: "Restore" })) &&
                api.restore(layout.id, v.version).then(() => (toast("Layout restored"), onChanged(), loadHistory())).catch(onError)
              }
            >
              Restore
            </button>
          </div>
        ))}
      </section>

      <section className="list">
        <div className="list-head">
          <h3>Media</h3>
          <button onClick={() => uploadInput.current?.click()}>+ Upload</button>
        </div>
        <input
          ref={uploadInput}
          type="file"
          accept="image/png,image/jpeg,image/bmp,image/gif"
          multiple
          hidden
          onChange={async (e) => {
            const files = [...(e.target.files ?? [])];
            e.target.value = "";
            for (const f of files) await api.uploadMedia(f).catch(onError);
            loadMedia();
          }}
        />
        {media.length === 0 && <small className="muted">Images for image layers and signal-loss slates (PNG, JPEG, BMP, GIF).</small>}
        {media.map((f) => (
          <div key={f.name} className="list-item">
            <img className="thumb" src={`/api/media/${encodeURIComponent(f.name)}`} alt="" />
            <div className="list-text">
              <div>{f.name}</div>
              <small>{kb(f.bytes)}</small>
            </div>
            <button title="Delete" onClick={async () => (await dialog.confirm(`Delete ${f.name}?`, { message: "Layers and slates that use it show nothing.", okLabel: "Delete", danger: true })) && api.deleteMedia(f.name).then(loadMedia).catch(onError)}>
              ✕
            </button>
          </div>
        ))}
      </section>

      <section className="list">
        <div className="list-head">
          <h3>Signal loss</h3>
        </div>
        <small className="muted">
          When an NDI stream sends nothing for 2 s, show this image in its place. A group can choose its own (group → Content). Without one, the last frame
          stays up.
        </small>
        <MediaSelect
          value={layout.fallbackImage ?? ""}
          placeholder="Keep the last frame"
          onError={onError}
          onChange={(name) => api.putLayout({ fallbackImage: name }).then(onChanged).catch(onError)}
        />
      </section>

      <section className="list">
        <div className="list-head">
          <h3>Access</h3>
        </div>
        <small className="muted">
          With a PIN or API key set, changes need one of them; looking stays open. Control systems send the API key as an <code>X-Api-Key</code> header.
        </small>
        <div className="list-item">
          <div className="list-text">
            <div>Editor PIN</div>
            <small>{security?.pinSet ? "set" : "not set"}</small>
          </div>
          <button onClick={() => setKey("editorPin", "editor PIN")}>{security?.pinSet ? "Change" : "Set"}</button>
        </div>
        <div className="list-item">
          <div className="list-text">
            <div>API key</div>
            <small>{security?.apiKeySet ? "set" : "not set"}</small>
          </div>
          <button onClick={() => setKey("apiKey", "API key")}>{security?.apiKeySet ? "Change" : "Set"}</button>
        </div>
        {storedKey() && (
          <button className="ghost" onClick={() => (storeKey(""), loadSecurity())} title="Forget the PIN in this browser">
            Lock this browser
          </button>
        )}
      </section>
    </>
  );
}

// Asks for the PIN when the engine refused a change.
export function UnlockDialog({ onClose }: { onClose: () => void }) {
  const [pin, setPin] = useState("");
  const [error, setError] = useState("");
  const submit = () => {
    storeKey(pin);
    api
      .checkKey()
      .then(onClose)
      .catch(() => (storeKey(""), setError("That PIN isn't right.")));
  };
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal" onClick={(e) => e.stopPropagation()}>
        <h2>Enter PIN</h2>
        <p className="muted">This engine is locked. Enter the editor PIN (or the API key) to make changes. It's remembered in this browser.</p>
        <input type="password" autoFocus value={pin} onChange={(e) => setPin(e.target.value)} onKeyDown={(e) => e.key === "Enter" && submit()} />
        {error && <small className="warn">{error}</small>}
        <div className="modal-buttons">
          <button className="ghost" onClick={onClose}>
            Cancel
          </button>
          <button className="primary" onClick={submit}>
            Unlock
          </button>
        </div>
      </div>
    </div>
  );
}
