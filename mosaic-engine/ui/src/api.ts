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
  crop: Rect; // region of the source drawn; w/h 0 = all of it
  opacity: number;
  color?: [number, number, number, number];
  animated?: boolean;
  enabled: boolean;
  status?: { status: string; inputFps?: number; width: number; height: number };
  group?: string; // wall group whose content this is (set by the group's presets)
}

// What one wall group shows; source rects are relative to the group's top-left corner.
export interface GroupPreset {
  id: string;
  name: string;
  sources: Source[];
}

export interface Output {
  id: string;
  name: string;
  ndiName: string;
  width: number;
  height: number;
  crop: Rect;
  rotation: 0 | 90 | 180 | 270;
  angle: number; // free tilt of the crop on the canvas, degrees clockwise
  scaleMode: ScaleMode;
  source: string;
  pixelFormat: "uyvy" | "bgra";
  group?: string; // id of the wall group that generated it
  enabled: boolean;
}

// A wall group: a grid of panels with bezel gaps (1 x 1 for a single display).
export interface Wall {
  id: string;
  name: string;
  cols: number;
  rows: number;
  outputWidth: number;
  outputHeight: number;
  rotation: 0 | 90 | 180 | 270;
  gapX: number;
  gapY: number;
  x: number;
  y: number;
  pixelFormat: "uyvy" | "bgra";
  fitCanvas: boolean;
  presets: GroupPreset[];
  activePreset: string;
}

// Where a wall group sits on the canvas, bezel gaps included.
export function wallBounds(w: Wall): Rect {
  const sideways = w.rotation === 90 || w.rotation === 270;
  const cw = sideways ? w.outputHeight : w.outputWidth;
  const ch = sideways ? w.outputWidth : w.outputHeight;
  return { x: w.x, y: w.y, w: w.cols * cw + (w.cols - 1) * w.gapX, h: w.rows * ch + (w.rows - 1) * w.gapY };
}

export interface Scene {
  id: string;
  name: string;
  sources: Source[];
}

export interface Layout {
  id: string;
  name: string;
  frameRate: number;
  canvas: { width: number; height: number; background: [number, number, number, number] };
  sources: Source[];
  outputs: Output[];
  walls: Wall[];
  scenes: Scene[];
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
  activate: (id: string, transitionMs = 0) => call("POST", `/api/layouts/${id}/activate`, { transitionMs }),
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
  createWall: (w: Partial<Wall>) => call<Layout>("POST", "/api/walls", w),
  putWall: (id: string, w: Partial<Wall>) => call<Layout>("PUT", `/api/walls/${id}`, w),
  moveWall: (id: string, x: number, y: number) => call<Layout>("POST", `/api/walls/${id}/move`, { x, y }),
  deleteWall: (id: string, removeOutputs: boolean) => call("DELETE", `/api/walls/${id}${removeOutputs ? "?outputs=delete" : ""}`),
  saveGroupPreset: (group: string, name: string) => call<GroupPreset>("POST", `/api/walls/${group}/presets`, { name }),
  recallGroupPreset: (group: string, id: string, transitionMs: number) =>
    call("POST", `/api/walls/${group}/presets/${id}/recall`, { transitionMs }),
  deleteGroupPreset: (group: string, id: string) => call("DELETE", `/api/walls/${group}/presets/${id}`),
  showOnGroup: (group: string, ndiName: string, transitionMs: number) => call("POST", `/api/walls/${group}/show`, { ndiName, transitionMs }),
  clearGroup: (group: string, transitionMs: number) => call("POST", `/api/walls/${group}/clear`, { transitionMs }),
  saveScene: (name: string) => call<Scene>("POST", "/api/scenes", { name }),
  recallScene: (id: string, transitionMs: number) => call("POST", `/api/scenes/${id}/recall`, { transitionMs }),
  deleteScene: (id: string) => call("DELETE", `/api/scenes/${id}`),
};
