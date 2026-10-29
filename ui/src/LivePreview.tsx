import { useEffect, useRef, useState } from "react";

// Polls a JPEG preview from the engine. The engine only renders previews
// that are being requested, so this also keeps the preview alive.
export function LivePreview({ target, intervalMs = 150, className }: { target: string; intervalMs?: number; className?: string }) {
  const [src, setSrc] = useState<string | null>(null);
  const urlRef = useRef<string | null>(null);

  useEffect(() => {
    let stopped = false;
    let timer: number | undefined;
    const tick = async () => {
      try {
        const res = await fetch(`/api/preview/${target}.jpg?t=${Date.now()}`, { cache: "no-store" });
        if (res.status === 200) {
          const url = URL.createObjectURL(await res.blob());
          if (urlRef.current) URL.revokeObjectURL(urlRef.current);
          urlRef.current = url;
          if (!stopped) setSrc(url);
        }
      } catch {
        /* engine restarting; keep trying */
      }
      if (!stopped) timer = window.setTimeout(tick, intervalMs);
    };
    tick();
    return () => {
      stopped = true;
      window.clearTimeout(timer);
    };
  }, [target, intervalMs]);

  useEffect(() => () => {
    if (urlRef.current) URL.revokeObjectURL(urlRef.current);
  }, []);

  return src ? <img className={className} src={src} alt={`${target} preview`} draggable={false} /> : <div className={`${className ?? ""} preview-empty`}>waiting for preview…</div>;
}
