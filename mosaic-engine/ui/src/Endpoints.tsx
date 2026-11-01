import { useState } from "react";
import { api, type EndpointDisplay, type EndpointsInfo, type EndpointView, type Layout, type Screen } from "./api";
import { TextField } from "./Inspector";

const screenLabel = (s: Screen) => `${s.short}${s.primary ? " (main)" : ""} · ${s.width}×${s.height}${s.monitor ? ` · ${s.monitor}` : ""}`;

function ago(s: number | null) {
  if (s == null) return "never";
  if (s < 60) return `${s}s ago`;
  if (s < 3600) return `${Math.round(s / 60)} min ago`;
  return `${Math.round(s / 3600)} h ago`;
}

// Sidebar list: endpoints with their health, and PCs that checked in but aren't set up yet.
export function EndpointList({
  info,
  selectedId,
  onSelect,
  onSetUp,
}: {
  info: EndpointsInfo | null;
  selectedId: string | null;
  onSelect: (id: string) => void;
  onSetUp: (host: string, screens: Screen[]) => void;
}) {
  return (
    <section className="list">
      <div className="list-head">
        <h3 title="Receiving PCs: set up here which output goes on each of their screens">Endpoints</h3>
        <button onClick={() => onSetUp("", [])}>+ Add</button>
      </div>
      {!info?.endpoints.length && !info?.unassigned.length && (
        <small className="muted">
          A receiving PC appears here once its dashboard has this PC set as <i>Engine PC</i>.
        </small>
      )}
      {info?.endpoints.map((e) => {
        const ok = e.displays.filter((d) => d.ok).length;
        const state = !e.online ? "off" : ok === e.displays.length ? "on" : "warn";
        return (
          <div key={e.id} className={`list-item${e.id === selectedId ? " selected" : ""}`} onClick={() => onSelect(e.id)}>
            <span className={`dot ${state}`} />
            <div className="list-text">
              <div>{e.name}</div>
              <small>
                {e.online ? `${ok}/${e.displays.length} display(s) playing` : `offline · seen ${ago(e.lastSeenSecondsAgo)}`}
              </small>
            </div>
          </div>
        );
      })}
      {info?.unassigned.map((u) => (
        <div key={u.host} className="list-item new-pc">
          <span className={`dot ${u.online ? "on" : "off"}`} />
          <div className="list-text">
            <div>{u.host}</div>
            <small>
              not set up · {u.screens.length} screen(s) · {u.address}
            </small>
          </div>
          <button onClick={() => onSetUp(u.host, u.screens)}>Set up</button>
        </div>
      ))}
    </section>
  );
}

// One endpoint: which output each display slot shows, on which screen, and whether it's playing.
export function EndpointPanel({
  layout,
  endpoint,
  onChanged,
  onError,
  onClose,
}: {
  layout: Layout;
  endpoint: EndpointView;
  onChanged: () => void;
  onError: (e: unknown) => void;
  onClose: () => void;
}) {
  const [groupPick, setGroupPick] = useState("");
  const slots: EndpointDisplay[] = endpoint.displays.map(({ screen, output }) => ({ screen, output }));
  const save = (displays: EndpointDisplay[]) => api.putEndpoint(endpoint.id, { displays }).then(onChanged).catch(onError);
  const setSlot = (i: number, p: Partial<EndpointDisplay>) => save(slots.map((d, j) => (j === i ? { ...d, ...p } : d)));
  const used = new Set(slots.map((d) => d.output));
  const nextFree = () => layout.outputs.find((o) => !used.has(o.id))?.id ?? layout.outputs[0]?.id ?? "";

  const outputOptions = (
    <>
      <option value="">— nothing —</option>
      {layout.walls.map((w) => (
        <optgroup key={w.id} label={w.name}>
          {layout.outputs
            .filter((o) => o.group === w.id)
            .map((o) => (
              <option key={o.id} value={o.id}>
                {o.name} · {o.ndiName}
              </option>
            ))}
        </optgroup>
      ))}
      <optgroup label="Standalone">
        {layout.outputs
          .filter((o) => !layout.walls.some((w) => w.id === o.group))
          .map((o) => (
            <option key={o.id} value={o.id}>
              {o.name} · {o.ndiName}
            </option>
          ))}
      </optgroup>
    </>
  );

  return (
    <div className="inspector">
      <div className="inspector-head">
        <h2>
          <span className={`dot ${endpoint.online ? "on" : "off"}`} /> {endpoint.name}
        </h2>
        <button onClick={onClose}>Close</button>
      </div>
      <small className="muted">
        {endpoint.online
          ? `Online · ${endpoint.address} · checked in ${ago(endpoint.lastSeenSecondsAgo)} · ${endpoint.screens.length} screen(s)`
          : `Offline · last seen ${ago(endpoint.lastSeenSecondsAgo)}. Its dashboard needs this PC as Engine PC.`}
      </small>

      <div className="group">
        <TextField label="Name" value={endpoint.name} onCommit={(name) => api.putEndpoint(endpoint.id, { name }).then(onChanged).catch(onError)} />
        <TextField label="PC name" value={endpoint.host} onCommit={(host) => api.putEndpoint(endpoint.id, { host }).then(onChanged).catch(onError)} />
      </div>

      <div className="group">
        <h3>Displays</h3>
        {endpoint.displays.length === 0 && <small className="muted wide-note">No displays yet. Add one, or a whole group at once.</small>}
        {endpoint.displays.map((d, i) => {
          const state = d.error
            ? d.error
            : !endpoint.online
              ? "PC offline"
              : d.ok
                ? "playing"
                : d.reachable === false
                  ? "Studio Monitor not answering"
                  : d.playing
                    ? `playing ${d.playing} instead`
                    : "starting…";
          return (
            <div key={i} className="endpoint-slot wide-note">
              <div className="slot-head">
                <span className={`dot ${d.ok ? "on" : endpoint.online ? "warn" : "off"}`} />
                <b>Display {d.index}</b>
                <small className="muted grow">{state}</small>
                <button title="Remove this display" onClick={() => save(slots.filter((_, j) => j !== i))}>
                  ✕
                </button>
              </div>
              <select value={d.output} onChange={(e) => setSlot(i, { output: e.target.value })}>
                {outputOptions}
              </select>
              <select value={d.screen} onChange={(e) => setSlot(i, { screen: e.target.value })}>
                <option value="">Screen: next free one</option>
                {endpoint.screens.map((s) => (
                  <option key={s.device} value={s.device}>
                    Screen: {screenLabel(s)}
                  </option>
                ))}
                {d.screen && !endpoint.screens.some((s) => s.device === d.screen) && <option value={d.screen}>Screen: {d.screen} (not on this PC)</option>}
              </select>
            </div>
          );
        })}
        <div className="row buttons">
          <button onClick={() => save([...slots, { screen: "", output: nextFree() }])}>+ Display</button>
          <select
            value={groupPick}
            onChange={(e) => {
              const w = layout.walls.find((x) => x.id === e.target.value);
              setGroupPick("");
              if (!w) return;
              // One display slot per panel of the group, in order (row by row).
              const add = layout.outputs.filter((o) => o.group === w.id).map((o) => ({ screen: "", output: o.id }));
              save([...slots, ...add]);
            }}
          >
            <option value="">+ Whole group…</option>
            {layout.walls.map((w) => (
              <option key={w.id} value={w.id}>
                {w.name} ({w.cols * w.rows})
              </option>
            ))}
          </select>
        </div>
        {endpoint.online && endpoint.screens.length > 0 && endpoint.displays.length > endpoint.screens.length && (
          <small className="warn wide-note">
            {endpoint.displays.length} displays but the PC has only {endpoint.screens.length} screen(s).
          </small>
        )}
      </div>

      <small className="muted">
        The PC sets itself up within a few seconds: one Studio Monitor per display, full screen on its screen, playing the
        output's stream. Its own dashboard then shows it as managed here.
      </small>
      <button className="danger" onClick={() => confirm(`Remove endpoint "${endpoint.name}"? The PC keeps showing what it shows now.`) && api.deleteEndpoint(endpoint.id).then(() => (onClose(), onChanged())).catch(onError)}>
        Remove endpoint
      </button>
    </div>
  );
}
