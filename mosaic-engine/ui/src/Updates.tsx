import { useEffect, useState } from "react";
import { dialog, toast } from "./Dialogs";
import { api, type UpdateInfo } from "./api";

const ago = (now: number, t: number) => {
  const s = Math.max(0, Math.round(now - t));
  return s < 90 ? `${s} s ago` : s < 5400 ? `${Math.round(s / 60)} min ago` : s < 129600 ? `${Math.round(s / 3600)} h ago` : `${Math.round(s / 86400)} days ago`;
};

// Settings → Updates: what each PC runs, and one button that updates the media server (git pull,
// rebuild, restart) and every receiving PC (git pull, restart; they fetch the new receiver from the
// engine). While the media server rebuilds the editor can't reach it; this keeps asking.
export function UpdatesSection({ onError }: { onError: (e: unknown) => void }) {
  const [info, setInfo] = useState<UpdateInfo | null>(null);
  const [unreachable, setUnreachable] = useState(false);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let alive = true;
    const load = () =>
      api
        .updates()
        .then((i) => alive && (setInfo(i), setUnreachable(false)))
        .catch(() => alive && setUnreachable(true));
    load();
    const t = setInterval(load, 3000);
    return () => {
      alive = false;
      clearInterval(t);
    };
  }, []);

  const start = async (target: "all" | "receivers") => {
    const ok = await dialog.confirm(target === "all" ? "Update the media server and every receiving PC?" : "Update every receiving PC?", {
      message:
        target === "all"
          ? "Each PC gets the latest version from GitHub. The receiving PCs restart their dashboard (the screens stay up). About 15 s later the media server stops the engine to rebuild it: the wall freezes and the editor is unreachable for a minute or two, then everything comes back by itself."
          : "Each receiving PC gets the latest version from GitHub and restarts its dashboard; the screens stay up.",
      okLabel: "Update",
      danger: target === "all",
    });
    if (!ok) return;
    setBusy(true);
    try {
      await api.startUpdate(target);
      toast(target === "all" ? "Updating: receiving PCs first, then the media server" : "Updating the receiving PCs");
    } catch (e) {
      onError(e);
    } finally {
      setBusy(false);
    }
  };

  const req = info?.request;
  const now = info?.now ?? Date.now() / 1000;
  const serverPending = !!req?.id && req.target !== "receivers" && !req.serverError && !(info?.server.lastUpdate && info.server.lastUpdate.time >= (req.requestedAt ?? 0));
  const recent = !!req?.requestedAt && now - req.requestedAt < 600;

  return (
    <section className="list">
      <div className="list-head">
        <h3>Updates</h3>
      </div>
      {unreachable && (
        <small className="warn">{recent || serverPending ? "The media server is rebuilding; the editor reconnects when it's back (usually 1–3 minutes)." : "Can't reach the media server."}</small>
      )}
      {info && (
        <>
          <div className="list-item">
            <span className={`dot ${unreachable ? "warn" : "on"}`} />
            <div className="list-text">
              <div>Media server ({info.server.host})</div>
              <small title={info.server.lastUpdate?.result}>
                {info.server.version || "version unknown"}
                {serverPending && recent ? " · updating…" : info.server.lastUpdate ? ` · last update ${ago(now, info.server.lastUpdate.time)}: ${info.server.lastUpdate.ok ? "ok" : "failed"}` : ""}
              </small>
              {req?.serverError && <small className="warn">{req.serverError}</small>}
              {info.server.lastUpdate && !info.server.lastUpdate.ok && <small className="warn">{info.server.lastUpdate.result}</small>}
            </div>
          </div>
          {info.endpoints.map((pc) => {
            const current = pc.update?.id === req?.id && !!req?.id;
            const behind = !!pc.version && !!info.server.version && pc.version !== info.server.version;
            const state = current ? pc.update?.state : undefined;
            return (
              <div className="list-item" key={pc.host}>
                <span className={`dot ${!pc.online ? "off" : state === "failed" || behind ? "warn" : "on"}`} />
                <div className="list-text">
                  <div>{pc.name}</div>
                  <small title={current ? pc.update?.message : undefined}>
                    {!pc.online ? "offline · " : ""}
                    {pc.version ? `${pc.version}${behind ? " · differs from the media server" : ""}` : "version unknown (update it once by hand)"}
                    {state === "updating" ? " · updating…" : state === "failed" ? " · update failed" : state === "updated" ? " · updated" : ""}
                  </small>
                  {state === "failed" && <small className="warn">{pc.update?.message}</small>}
                </div>
              </div>
            );
          })}
          {!info.endpoints.length && <small className="muted">No receiving PC has checked in yet.</small>}
          <small className="muted">
            Receiving PCs download the Mosaic Receiver from the media server by themselves after it rebuilds. The first time, run <code>update.bat</code> on each PC by
            hand: older versions don't have this button's side.
          </small>
          <div className="row-buttons">
            <button className="primary" disabled={busy || unreachable || !info.server.canUpdate} onClick={() => start("all")}>
              Update everything
            </button>
            <button disabled={busy || unreachable || !info.endpoints.length} onClick={() => start("receivers")}>
              PCs only
            </button>
          </div>
        </>
      )}
    </section>
  );
}
