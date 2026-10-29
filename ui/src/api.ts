// Typed client for the engine's REST API.

export type Rect = { x: number; y: number; w: number; h: number };
export type ScaleMode = "stretch" | "fit" | "fill";
export type SourceType = "test_pattern" | "ndi" | "solid";

export interface Source {
  id: string;
  name: string;
  type: SourceType;
  ndiName?: string;
  rect: Rect;
  color?: [number, number, number, number];
  enabled: boolean;
  status?: { status: string; inputFps?: number; width: number; height: number };
}

export interface Output {
  id: string;
  name: string;
  ndiName: string;
  width: number;
  height: number;
  crop: Rect;
  rotation: 0 | 90 | 180 | 270;
  scaleMode: ScaleMode;
  source: string;
  pixelFormat: "uyvy" | "bgra";
  enabled: boolean;
}

export interface Layout {
  id: string;
  name: string;
  frameRate: number;
  canvas: { width: number; height: number; background: [number, number, number, number] };
  sources: Source[];
  outputs: Output[];
}

export interface LayoutSummary {
  id: string;
  name: string;
  outputs: number;
  active: boolean;
}

export interface OutputMetrics {
  id: string;
  name: string;
  ndiName: string;
  sink: string;
  pixelFormat: string;
  enabled: boolean;
  width: number;
  height: number;
  fps: number;
  framesSent: number;
  droppedNoBuffer: number;
  droppedQueue: number;
  queueDepth: number;
  buffersInUse: number;
  sendMs: number;
  sendMsMax: number;
  latencyMs: number;
  lastFrameId: number;
  connections: number;
}

export interface Metrics {
  engine: {
    backend: string;
    device: string;
    targetFps: number;
    renderFps: number;
    frameId: number;
    encodeMs: number;
    gpuWaitMs: number;
    gpuMs: number;
    frameMs: number;
    frameMsMax: number;
    gpuUtilization: number;
    readbackMBps: number;
    tickJitterMs: number;
    lateFrames: number;
    ndi: string;
    layout: string;
  };
  sources: { id: string; type: string; status: string; inputFps?: number; width: number; height: number }[];
  outputs: OutputMetrics[];
  sync: { framesComplete: number; framesPartial: number; completeRatio: number; lastFrameIdSpread: number };
}

async function call<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, {
    method,
    headers: body !== undefined ? { "Content-Type": "application/json" } : undefined,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error((data as { error?: string }).error ?? res.statusText);
  return data as T;
}

export const api = {
  layout: () => call<Layout>("GET", "/api/layout"),
  putLayout: (patch: Partial<Layout>) => call<Layout>("PUT", "/api/layout", patch),
  layouts: () => call<{ layouts: LayoutSummary[] }>("GET", "/api/layouts").then((r) => r.layouts),
  activate: (id: string) => call("POST", `/api/layouts/${id}/activate`),
  saveAs: (name: string) => call<Layout>("POST", "/api/layouts", { fromActive: true, name }),
  deleteLayout: (id: string) => call("DELETE", `/api/layouts/${id}`),
  addOutput: (o: Partial<Output>) => call<Output>("POST", "/api/outputs", o),
  patchOutput: (id: string, patch: Partial<Output>) => call<Output>("PATCH", `/api/outputs/${id}`, patch),
  deleteOutput: (id: string) => call("DELETE", `/api/outputs/${id}`),
  sources: () =>
    call<{ sources: Source[]; ndi: { available: boolean; discovered: string[] } }>("GET", "/api/sources"),
  addSource: (s: Partial<Source>) => call<Source>("POST", "/api/sources", s),
  patchSource: (id: string, patch: Partial<Source>) => call<Source>("PATCH", `/api/sources/${id}`, patch),
  deleteSource: (id: string) => call("DELETE", `/api/sources/${id}`),
};
