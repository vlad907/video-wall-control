import { useState } from "react";
import { dialog } from "./Dialogs";
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
// What held a freeze up, in plain words (the receiver's log has the details).
const freezeCause: Record<string, string> = {
  network: "no frames arriving from the media server",
  wait: "Windows / the GPU didn't take a new frame",
  upload: "copying pictures to the GPU was slow",
  present: "drawing was slow (GPU or driver busy)",
  screen: "the monitors skipped refreshes",
  loop: "the receiver didn't get CPU time",
};

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
  receiverVersion,
  onChanged,
  onError,
  onClose,
}: {
  layout: Layout;
  endpoint: EndpointView;
  receiverVersion?: string; // the version the engine hands out
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
        <label className="field wide">
          <span>Player</span>
          <select
            value={endpoint.player ?? "studio_monitor"}
            onChange={(e) => api.putEndpoint(endpoint.id, { player: e.target.value as "studio_monitor" | "mosaic" }).then(onChanged).catch(onError)}
          >
            <option value="studio_monitor">NDI Studio Monitor (one window per display)</option>
            <option value="mosaic">Mosaic Receiver (displays in step)</option>
          </select>
        </label>
        {endpoint.player === "mosaic" && endpoint.receiver && (
          <small className={`wide-note ${endpoint.receiver.error || endpoint.receiver.spreadMax10s > 1 ? "warn" : "muted"}`}>
            {endpoint.receiver.error
              ? `Receiver: ${endpoint.receiver.error}`
              : `In step ${(endpoint.receiver.alignedRatio * 100).toFixed(1)}% of refreshes · at most ${endpoint.receiver.spreadMax10s} frame(s) apart (last 10 s) · presenting ${Math.round(endpoint.receiver.presentFps ?? 0)} of ${Math.round(endpoint.receiver.refreshHz)} Hz · upload ${(endpoint.receiver.uploadMs ?? 0).toFixed(1)} ms · ${endpoint.receiver.presenter}`}
          </small>
        )}
        {endpoint.player === "mosaic" && endpoint.receiver && (
          <div className="wide-note sync-box">
            <div className="sync-line">
              <span>On screen, displays apart</span>
              <b className={(endpoint.receiver.screenSpreadNowMs ?? endpoint.receiver.screenSpreadMs ?? 0) > 4 ? "warn" : ""}>
                {endpoint.receiver.timing === "measured"
                  ? `${(endpoint.receiver.screenSpreadNowMs ?? endpoint.receiver.screenSpreadMs ?? 0).toFixed(1)} ms now · ${(endpoint.receiver.screenSpreadMaxMs ?? 0).toFixed(1)} ms max (10 s)`
                  : "not measurable on this PC"}
              </b>
            </div>
            {(() => {
              // Displays Windows shows a refresh later (composited) and the hold-back that matches them.
              const late = endpoint.displays.filter((d) => (d.windowsLateMs ?? 0) > 8).map((d) => d.index);
              const held = endpoint.displays.filter((d) => (d.heldRefreshes ?? 0) > 0).map((d) => d.index);
              if (!late.length) return null;
              return (
                <small className="muted">
                  Windows shows display{late.length > 1 ? "s" : ""} {late.join(", ")} a refresh late (it composites {late.length > 1 ? "them" : "it"}: HDR on, an
                  overlay, the taskbar…).{" "}
                  {held.length
                    ? `The receiver holds display${held.length > 1 ? "s" : ""} ${held.join(", ")} back to match, so all show each frame together.`
                    : endpoint.receiver?.compensate === false
                      ? "Compensation is off."
                      : "Matching it in a few seconds…"}
                </small>
              );
            })()}
            {!!endpoint.receiver.bufferMs && (
              <div className="sync-line">
                <span>Buffer</span>
                <b className={endpoint.receiver.bufferReady ? "" : "warn"}>
                  {endpoint.receiver.bufferReady
                    ? `${endpoint.receiver.bufferMs} ms · frames held ${Math.round(endpoint.receiver.heldMs ?? 0)} ms · ${endpoint.receiver.gapsAbsorbed ?? 0} network stall(s) hidden`
                    : `${endpoint.receiver.bufferMs} ms · waiting for the media server's clock…`}
                </b>
              </div>
            )}
            {endpoint.receiver.freezesTotal !== undefined && (
              <>
                <div className="sync-line">
                  <span>Freezes (screens held a picture)</span>
                  <b className={(endpoint.receiver.freezesLastMinute ?? 0) > 0 ? "warn" : ""}>
                    {endpoint.receiver.freezesLastMinute ?? 0} in the last minute · {endpoint.receiver.freezesTotal} since start
                  </b>
                </div>
                {(endpoint.receiver.freezes ?? []).slice(0, 5).map((f, i) => (
                  <small key={i} className="muted">
                    {f.at.slice(11, 19)} · {Math.round(f.ms)} ms · {freezeCause[f.cause] ?? f.cause}
                  </small>
                ))}
              </>
            )}
            {endpoint.diagnostics?.gpu && (
              <div className="sync-line">
                <span>GPU</span>
                {endpoint.diagnostics.gpu.error ? (
                  <b className="muted">{endpoint.diagnostics.gpu.error}</b>
                ) : (
                  <b className={(endpoint.diagnostics.gpu.utilMax60 ?? 0) >= 95 || endpoint.diagnostics.gpu.throttle60?.length ? "warn" : ""}>
                    {endpoint.diagnostics.gpu.util}% now · {endpoint.diagnostics.gpu.utilMax60}% max (1 min) ·{" "}
                    {((endpoint.diagnostics.gpu.memUsedMB ?? 0) / 1024).toFixed(1)}/{((endpoint.diagnostics.gpu.memTotalMB ?? 0) / 1024).toFixed(1)} GB ·{" "}
                    {endpoint.diagnostics.gpu.tempC} °C
                    {endpoint.diagnostics.gpu.throttle60?.length ? ` · slowed: ${endpoint.diagnostics.gpu.throttle60.join(", ")}` : ""}
                  </b>
                )}
              </div>
            )}
            {endpoint.diagnostics && (
              <>
                <div className="sync-line">
                  <span>Display driver (last hour)</span>
                  <b className={endpoint.diagnostics.driverEvents?.length || endpoint.diagnostics.screenChanges?.length ? "warn" : ""}>
                    {endpoint.diagnostics.driverEventsError
                      ? "can't read the Windows event log"
                      : `${endpoint.diagnostics.driverEvents?.length ?? 0} event(s) · ${endpoint.diagnostics.screenChanges?.length ?? 0} screen change(s)`}
                  </b>
                </div>
                {[
                  ...(endpoint.diagnostics.driverEvents ?? []).map((e) => ({ at: e.at, text: `${e.source} ${e.id}: ${e.message}` })),
                  ...(endpoint.diagnostics.screenChanges ?? []).map((c) => ({ at: c.at, text: `screens changed: ${c.change}` })),
                ]
                  .sort((a, b) => b.at.localeCompare(a.at))
                  .slice(0, 5)
                  .map((e, i) => (
                    <small key={i} className="muted">
                      {e.at.slice(11, 19)} · {e.text}
                    </small>
                  ))}
              </>
            )}
            <label className="toggle">
              <input
                type="checkbox"
                checked={!!endpoint.flashTest}
                onChange={(e) => api.putEndpoint(endpoint.id, { flashTest: e.target.checked }).then(onChanged).catch(onError)}
              />
              Flash test: all displays flash white together twice a second; film it in slow motion
            </label>
          </div>
        )}
        {endpoint.player === "mosaic" && !endpoint.receiver && endpoint.online && !endpoint.receiverUpdate && (
          <small className="wide-note muted">Starting the Mosaic Receiver on the PC…</small>
        )}
        {endpoint.receiverUpdate ? (
          <small className="wide-note warn">Receiver: {endpoint.receiverUpdate}</small>
        ) : endpoint.online && receiverVersion ? (
          <small className="wide-note muted">
            {endpoint.receiverVersion === receiverVersion
              ? `Receiver up to date (${receiverVersion.slice(0, 8)})`
              : endpoint.receiverVersion
                ? `Receiver updating to ${receiverVersion.slice(0, 8)} (has ${endpoint.receiverVersion.slice(0, 8)})…`
                : "Receiver: downloading it from this media server…"}
          </small>
        ) : null}
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
                <small className="muted grow">
                  {state}
                  {d.fps !== undefined && endpoint.player === "mosaic" ? ` · ${d.fps.toFixed(0)} fps` : ""}
                  {endpoint.player === "mosaic" && d.offsetMaxMs !== undefined && endpoint.receiver?.timing === "measured"
                    ? ` · +${(d.offsetMs ?? 0).toFixed(1)} ms (max ${d.offsetMaxMs.toFixed(1)})`
                    : ""}
                  {d.heldRefreshes ? ` · held back ${d.heldRefreshes} refresh${d.heldRefreshes > 1 ? "es" : ""}` : ""}
                  {d.missedRefreshes ? ` · ${d.missedRefreshes} repeated` : ""}
                  {d.shownFrameId ? ` · frame ${d.shownFrameId}` : ""}
                  {d.late ? ` · ${d.late} late` : ""}
                </small>
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
      <button className="danger" onClick={async () => (await dialog.confirm(`Remove endpoint "${endpoint.name}"?`, { message: "The PC keeps showing what it shows now, and goes back to manual setup.", okLabel: "Remove", danger: true })) && api.deleteEndpoint(endpoint.id).then(() => (onClose(), onChanged())).catch(onError)}>
        Remove endpoint
      </button>
    </div>
  );
}
