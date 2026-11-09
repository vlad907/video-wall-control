import { useEffect, useState } from "react";
import { api, type AudioInfo, type AudioOutput, type Layout } from "./api";
import { TextField } from "./Inspector";

// A level meter: two bars (left, right), -60..0 dBFS, red near the top.
function Meter({ db }: { db?: number[] }) {
  return (
    <div className="audio-meter" title={db ? `${db[0]} / ${db[1]} dBFS` : undefined}>
      {[0, 1].map((i) => {
        const v = db?.[i] ?? -100;
        return (
          <span key={i}>
            <i className={v > -3 ? "hot" : v > -12 ? "warm" : ""} style={{ width: `${Math.max(0, Math.min(100, ((v + 60) / 60) * 100))}%` }} />
          </span>
        );
      })}
    </div>
  );
}

// Settings → Audio: the media server's sound outputs. Each plays one NDI source's audio on one of
// the PC's sound devices (a channel pair of it), delayed to match the wall, with its own volume.
export function AudioSection({ layout, onChanged, onError }: { layout: Layout; onChanged: () => void; onError: (e: unknown) => void }) {
  const [info, setInfo] = useState<AudioInfo | null>(null);
  const outputs = layout.audioOutputs ?? [];

  useEffect(() => {
    let alive = true;
    const load = () =>
      api
        .audio()
        .then((i) => alive && setInfo(i))
        .catch(() => {});
    load();
    const t = setInterval(load, 500);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, []);

  const save = (next: AudioOutput[]) => api.putLayout({ audioOutputs: next }).then(onChanged).catch(onError);
  const patch = (id: string, p: Partial<AudioOutput>) => save(outputs.map((o) => (o.id === id ? { ...o, ...p } : o)));

  // NDI sources to pick from: the layout's own, then anything else on the network.
  const named = layout.sources.filter((s) => s.type === "ndi" && s.ndiName);
  const sourceNames = [...new Set([...named.map((s) => s.ndiName!), ...(info?.ndiSources ?? [])])];
  const label = (ndi: string) => {
    const s = named.find((x) => x.ndiName === ndi);
    return s && s.name !== ndi ? `${s.name} (${ndi})` : ndi;
  };

  return (
    <section className="list">
      <div className="list-head">
        <h3>Audio</h3>
        <button
          onClick={() =>
            save([
              ...outputs,
              {
                id: "",
                name: `Audio ${outputs.length + 1}`,
                device: info?.devices.find((d) => d.default)?.id ?? "",
                source: named[0]?.ndiName ?? "",
                channelPair: 0,
                volumeDb: 0,
                mute: false,
                matchWall: true,
                delayMs: 0,
                enabled: true,
              },
            ])
          }
        >
          + Output
        </button>
      </div>
      <small className="muted">
        Plays an NDI source's sound on one of the media server's sound devices, delayed to match the wall
        {info ? ` (${Math.round(info.wallDelayMs)} ms now: the receivers' buffer plus the media server's and receivers' own delay)` : ""}. The TVs add their own
        processing on top: clap in front of the wall and adjust the trim until sound and picture line up.
      </small>
      {info?.devicesError && <small className="warn">{info.devicesError}</small>}
      {outputs.map((o) => {
        const st = info?.outputs.find((s) => s.id === o.id);
        const dev = info?.devices.find((d) => d.id === o.device);
        const pairs = Math.max(1, Math.floor((dev?.channels ?? 2) / 2));
        const problem = st?.error || st?.sourceError;
        return (
          <div className="audio-output" key={o.id}>
            <div className="audio-row">
              <TextField label="Name" value={o.name} onCommit={(name) => patch(o.id, { name })} />
              <label className="toggle" title="Plays when on">
                <input type="checkbox" checked={o.enabled} onChange={(e) => patch(o.id, { enabled: e.target.checked })} />
                On
              </label>
              <button className="ghost" title="Remove this output" onClick={() => save(outputs.filter((x) => x.id !== o.id))}>
                ✕
              </button>
            </div>
            <label className="field wide">
              <span>Source</span>
              <select value={o.source} onChange={(e) => patch(o.id, { source: e.target.value })}>
                <option value="">Choose an NDI source…</option>
                {sourceNames.map((n) => (
                  <option key={n} value={n}>
                    {label(n)}
                  </option>
                ))}
                {o.source && !sourceNames.includes(o.source) && <option value={o.source}>{o.source} (not seen)</option>}
              </select>
            </label>
            <div className="audio-row">
              <label className="field wide">
                <span>Sound device</span>
                <select
                  value={o.device}
                  onChange={(e) => patch(o.id, { device: e.target.value, deviceName: info?.devices.find((d) => d.id === e.target.value)?.name ?? "", channelPair: 0 })}
                >
                  <option value="">Windows default</option>
                  {info?.devices.map((d) => (
                    <option key={d.id} value={d.id}>
                      {d.name}
                      {d.channels > 2 ? ` (${d.channels} ch)` : ""}
                    </option>
                  ))}
                  {o.device && info && !dev && <option value={o.device}>{o.deviceName || "a device"} (not connected)</option>}
                </select>
              </label>
              {pairs > 1 && (
                <label className="field">
                  <span>Channels</span>
                  <select value={o.channelPair} onChange={(e) => patch(o.id, { channelPair: +e.target.value })}>
                    {Array.from({ length: pairs }, (_, i) => (
                      <option key={i} value={i}>
                        {i * 2 + 1}–{i * 2 + 2}
                      </option>
                    ))}
                  </select>
                </label>
              )}
            </div>
            <div className="audio-row">
              <label className="field wide">
                <span>
                  Volume {o.mute ? "(muted)" : `${o.volumeDb > 0 ? "+" : ""}${o.volumeDb} dB`}
                </span>
                <input
                  type="range"
                  min={-60}
                  max={12}
                  step={1}
                  defaultValue={o.volumeDb}
                  key={`${o.id}-${o.volumeDb}`}
                  onPointerUp={(e) => patch(o.id, { volumeDb: +(e.target as HTMLInputElement).value })}
                  onKeyUp={(e) => patch(o.id, { volumeDb: +(e.target as HTMLInputElement).value })}
                />
              </label>
              <label className="toggle">
                <input type="checkbox" checked={o.mute} onChange={(e) => patch(o.id, { mute: e.target.checked })} />
                Mute
              </label>
            </div>
            <div className="audio-row">
              <label className="toggle" title="Delay it as much as the wall's picture, plus the trim">
                <input type="checkbox" checked={o.matchWall} onChange={(e) => patch(o.id, { matchWall: e.target.checked, delayMs: e.target.checked ? 0 : Math.round(st?.delayMs ?? info?.wallDelayMs ?? 0) })} />
                Match the wall
              </label>
              <label className="field">
                <span>{o.matchWall ? "Trim (ms)" : "Delay (ms)"}</span>
                <input
                  type="number"
                  step={5}
                  min={o.matchWall ? -1000 : 0}
                  max={5000}
                  defaultValue={o.delayMs}
                  key={`${o.id}-${o.delayMs}-${o.matchWall}`}
                  onBlur={(e) => +e.target.value !== o.delayMs && patch(o.id, { delayMs: +e.target.value })}
                  onKeyDown={(e) => e.key === "Enter" && (e.target as HTMLInputElement).blur()}
                />
              </label>
            </div>
            <div className="audio-row">
              <Meter db={st?.levelDb} />
              <small className={problem ? "warn" : "muted"}>
                {!o.enabled
                  ? "off"
                  : !st
                    ? o.source
                      ? "starting…"
                      : "choose a source"
                    : problem
                      ? problem
                      : `${st.device || "default device"} · delayed ${Math.round(st.delayMs)} ms${st.jumps ? ` · ${st.jumps} skip(s)` : ""}`}
              </small>
            </div>
          </div>
        );
      })}
      {!outputs.length && <small className="muted">No audio outputs yet.</small>}
    </section>
  );
}
