import { useEffect, useRef, useState } from "react";

// In-app replacements for prompt() / confirm() / alert(), plus toasts.
// dialog.*() return promises, so callers read like the browser versions: `if (await dialog.confirm(...))`.

interface Request {
  kind: "prompt" | "confirm" | "alert";
  title: string;
  message?: string;
  value?: string;
  placeholder?: string;
  choices?: { value: string; label: string }[]; // prompt: pick one instead of typing
  okLabel?: string;
  danger?: boolean;
  allowEmpty?: boolean; // prompt: OK with nothing typed gives "" (cancel gives null)
  password?: boolean;
  resolve: (v: string | boolean | null) => void;
}

interface Toast {
  id: number;
  text: string;
  kind: "ok" | "error";
}

let show: ((r: Request) => void) | null = null;
let addToast: ((t: Omit<Toast, "id">) => void) | null = null;

const open = <T,>(r: Omit<Request, "resolve">) =>
  new Promise<T>((resolve) => (show ? show({ ...r, resolve: resolve as Request["resolve"] }) : resolve(null as T)));

export const dialog = {
  prompt: (
    title: string,
    o: { message?: string; value?: string; placeholder?: string; okLabel?: string; choices?: Request["choices"]; allowEmpty?: boolean; password?: boolean } = {},
  ) =>
    open<string | null>({ kind: "prompt", title, ...o }),
  confirm: (title: string, o: { message?: string; okLabel?: string; danger?: boolean } = {}) => open<boolean>({ kind: "confirm", title, ...o }),
  alert: (title: string, message?: string) => open<boolean>({ kind: "alert", title, message }),
};

export const toast = (text: string, kind: Toast["kind"] = "ok") => addToast?.({ text, kind });

export function DialogHost() {
  const [queue, setQueue] = useState<Request[]>([]);
  const [toasts, setToasts] = useState<Toast[]>([]);
  const [value, setValue] = useState("");
  const input = useRef<HTMLInputElement & HTMLSelectElement>(null);
  const current = queue[0];

  useEffect(() => {
    show = (r) => setQueue((q) => [...q, r]);
    let n = 0;
    addToast = (t) => {
      const id = ++n;
      setToasts((list) => [...list.slice(-3), { ...t, id }]);
      window.setTimeout(() => setToasts((list) => list.filter((x) => x.id !== id)), t.kind === "error" ? 6000 : 3000);
    };
    return () => {
      show = null;
      addToast = null;
    };
  }, []);

  useEffect(() => {
    if (!current) return;
    setValue(current.value ?? current.choices?.[0]?.value ?? "");
    window.setTimeout(() => (input.current?.focus(), input.current?.select?.()), 0);
  }, [current]);

  const close = (result: string | boolean | null) => {
    current?.resolve(result);
    setQueue((q) => q.slice(1));
  };
  const ok = () => close(current?.kind === "prompt" ? (current.allowEmpty ? value.trim() : value.trim() || null) : true);
  const cancel = () => close(current?.kind === "prompt" ? null : false);

  return (
    <>
      {current && (
        <div className="modal-backdrop" onMouseDown={cancel}>
          <div
            className="modal"
            onMouseDown={(e) => e.stopPropagation()}
            onKeyDown={(e) => {
              if (e.key === "Escape") cancel();
              if (e.key === "Enter" && current.kind !== "prompt") ok();
            }}
          >
            <h2>{current.title}</h2>
            {current.message && <p className="muted">{current.message}</p>}
            {current.kind === "prompt" &&
              (current.choices ? (
                <select ref={input} value={value} onChange={(e) => setValue(e.target.value)}>
                  {current.choices.map((c) => (
                    <option key={c.value} value={c.value}>
                      {c.label}
                    </option>
                  ))}
                </select>
              ) : (
                <input ref={input} type={current.password ? "password" : "text"} value={value} placeholder={current.placeholder} onChange={(e) => setValue(e.target.value)} onKeyDown={(e) => e.key === "Enter" && ok()} />
              ))}
            <div className="modal-buttons">
              {current.kind !== "alert" && (
                <button className="ghost" onClick={cancel}>
                  Cancel
                </button>
              )}
              <button className={current.danger ? "danger solid" : "primary"} onClick={ok} autoFocus={current.kind !== "prompt"}>
                {current.okLabel ?? (current.kind === "alert" ? "OK" : current.kind === "confirm" ? "Confirm" : "Save")}
              </button>
            </div>
          </div>
        </div>
      )}
      <div className="toasts">
        {toasts.map((t) => (
          <div key={t.id} className={`toast ${t.kind}`}>
            {t.text}
          </div>
        ))}
      </div>
    </>
  );
}
