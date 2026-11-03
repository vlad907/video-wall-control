// Typed client for the engine's REST API.

export type Rect = { x: number; y: number; w: number; h: number };
export type ScaleMode = "stretch" | "fit" | "fill";
export type SourceType = "test_pattern" | "ndi" | "solid" | "image" | "video";

export interface VideoStatus {
  width: number;
  height: number;
  fps: number;
  codec: string;
  hardware: boolean;
  durationS: number;
  positionS: number;
  decodeFps: number;
  playing: boolean;
}

export interface Source {
  id: string;
  name: string;
  type: SourceType;
  ndiName?: string;
  image?: string; // media file name (type "image")
  video?: string; // media file name (type "video")
  loop?: boolean; // video: start again at the end
  playing?: boolean; // video: false = paused
  rect: Rect;
  crop: Rect; // region of the source drawn; w/h 0 = all of it
  opacity: number;
  color?: [number, number, number, number];
  animated?: boolean;
  enabled: boolean;
  status?: { status: string; inputFps?: number; width: number; height: number; signal?: "ok" | "lost"; showing?: string; video?: VideoStatus };
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
  fallbackImage?: string; // media file shown where one of its streams has no signal
}

// Where a wall group sits on the canvas, bezel gaps included.
export function wallBounds(w: Wall): Rect {
  const sideways = w.rotation === 90 || w.rotation === 270;
  const cw = sideways ? w.outputHeight : w.outputWidth;
  const ch = sideways ? w.outputWidth : w.outputHeight;
  return { x: w.x, y: w.y, w: w.cols * cw + (w.cols - 1) * w.gapX, h: w.rows * ch + (w.rows - 1) * w.gapY };
}

// A receiving PC and which output goes on each of its screens.
export interface EndpointDisplay {
  screen: string; // Windows display device ("\\.\DISPLAY2"); "" = next free screen
  output: string; // output id
}

export interface Endpoint {
  id: string;
  name: string;
  host: string; // the PC's name
  player?: "studio_monitor" | "mosaic"; // what shows the streams on that PC
  displays: EndpointDisplay[];
}

export interface Screen {
  device: string;
  short: string;
  monitor?: string;
  width: number;
  height: number;
  x: number;
  y: number;
  primary?: boolean;
}

// An endpoint with the status from its last check-in.
export interface EndpointView extends Omit<Endpoint, "displays"> {
  online: boolean;
  lastSeenSecondsAgo: number | null;
  address: string;
  version: string;
  screens: Screen[];
  receiver?: { spreadMax10s: number; alignedRatio: number; refreshHz: number; presenter: string; error: string };
  displays: (EndpointDisplay & {
    shownFrameId?: number;
    late?: number;
    index: number;
    label?: string;
    stream?: string;
    error?: string;
    reachable?: boolean;
    playing?: string;
    onScreen?: string;
    ok: boolean;
  })[];
}

export interface EndpointsInfo {
  endpoints: EndpointView[];
  unassigned: { host: string; address: string; screens: Screen[]; online: boolean }[];
  engineHost: string;
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
  endpoints: Endpoint[];
  fallbackImage?: string; // default for streams with no signal
}

export interface LayoutVersion {
  version: string;
  time: string;
  name: string;
  outputs: number;
}

export interface MediaFile {
  name: string;
  bytes: number;
  kind?: "image" | "video";
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
  sources: { id: string; type: string; status: string; inputFps?: number; width: number; height: number; signal?: "ok" | "lost"; showing?: string; video?: VideoStatus }[];
  outputs: OutputMetrics[];
  sync: { framesComplete: number; framesPartial: number; completeRatio: number; lastFrameIdSpread: number };
}

// The PIN or API key, when the engine has one set (kept in this browser only).
const KEY = "mosaic.key";
export function storedKey() {
  try {
    return localStorage.getItem(KEY) ?? "";
  } catch {
    return "";
  }
}
export function storeKey(key: string) {
  try {
    if (key) localStorage.setItem(KEY, key);
    else localStorage.removeItem(KEY);
  } catch {
    /* private window: the key lasts until reload */
  }
}

// Raised when a change is refused for lack of a PIN; the app then asks for one.
export class LockedError extends Error {}

async function call<T>(method: string, path: string, body?: unknown, raw?: Blob): Promise<T> {
  const headers: Record<string, string> = {};
  if (body !== undefined) headers["Content-Type"] = "application/json";
  if (raw) headers["Content-Type"] = raw.type || "application/octet-stream";
  const key = storedKey();
  if (key) headers["X-Api-Key"] = key;
  const res = await fetch(path, {
    method,
    headers,
    body: raw ?? (body !== undefined ? JSON.stringify(body) : method === "GET" ? undefined : "{}"),
  });
  const data = await res.json().catch(() => ({}));
  if (res.status === 401) {
    window.dispatchEvent(new Event("mosaic-locked"));
    throw new LockedError("Enter the PIN to make changes.");
  }
  if (!res.ok) throw new Error((data as { error?: string }).error ?? res.statusText);
  return data as T;
}

export const api = {
  security: () => call<{ pinSet: boolean; apiKeySet: boolean; locked: boolean }>("GET", "/api/security"),
  putSecurity: (s: { editorPin?: string; apiKey?: string }) => call<{ pinSet: boolean; apiKeySet: boolean }>("PUT", "/api/security", s),
  checkKey: () => call("POST", "/api/security/check", {}),
  history: (layoutId: string) => call<{ history: LayoutVersion[] }>("GET", `/api/layouts/${layoutId}/history`).then((r) => r.history),
  restore: (layoutId: string, version: string) => call("POST", `/api/layouts/${layoutId}/history/${version}/restore`, {}),
  getLayout: (id: string) => call<Layout>("GET", `/api/layouts/${id}`),
  importLayout: (l: Layout) => call<Layout>("POST", "/api/layouts", l),
  media: () => call<{ files: MediaFile[]; folder: string }>("GET", "/api/media").then((r) => r.files),
  mediaFolder: () => call<{ files: MediaFile[]; folder: string }>("GET", "/api/media").then((r) => r.folder),
  // XHR rather than fetch, for upload progress (8K videos take a while).
  uploadMedia: (file: File, onProgress?: (fraction: number) => void) =>
    new Promise<MediaFile>((resolve, reject) => {
      const x = new XMLHttpRequest();
      x.open("POST", `/api/media?name=${encodeURIComponent(file.name)}`);
      x.setRequestHeader("Content-Type", file.type || "application/octet-stream");
      const key = storedKey();
      if (key) x.setRequestHeader("X-Api-Key", key);
      x.upload.onprogress = (e) => e.lengthComputable && onProgress?.(e.loaded / e.total);
      x.onload = () => {
        let data: { error?: string } = {};
        try {
          data = JSON.parse(x.responseText);
        } catch {
          /* not JSON */
        }
        if (x.status === 401) {
          window.dispatchEvent(new Event("mosaic-locked"));
          reject(new LockedError("Enter the PIN to make changes."));
        } else if (x.status >= 300) reject(new Error(data.error ?? x.statusText));
        else resolve(data as MediaFile);
      };
      x.onerror = () => reject(new Error(`Uploading ${file.name} failed (connection lost?)`));
      x.send(file);
    }),
  restartSource: (id: string) => call("POST", `/api/sources/${id}/restart`, {}),
  restartWall: (id: string) => call("POST", `/api/walls/${id}/restart`, {}),
  deleteMedia: (name: string) => call("DELETE", `/api/media/${encodeURIComponent(name)}`),
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
  patchWall: (id: string, p: { name?: string; fallbackImage?: string }) => call<Wall>("PATCH", `/api/walls/${id}`, p),
  moveWall: (id: string, x: number, y: number) => call<Layout>("POST", `/api/walls/${id}/move`, { x, y }),
  deleteWall: (id: string, removeOutputs: boolean) => call("DELETE", `/api/walls/${id}${removeOutputs ? "?outputs=delete" : ""}`),
  saveGroupPreset: (group: string, name: string) => call<GroupPreset>("POST", `/api/walls/${group}/presets`, { name }),
  recallGroupPreset: (group: string, id: string, transitionMs: number) =>
    call("POST", `/api/walls/${group}/presets/${id}/recall`, { transitionMs }),
  deleteGroupPreset: (group: string, id: string) => call("DELETE", `/api/walls/${group}/presets/${id}`),
  showOnGroup: (group: string, ndiName: string, transitionMs: number) => call("POST", `/api/walls/${group}/show`, { ndiName, transitionMs }),
  clearGroup: (group: string, transitionMs: number) => call("POST", `/api/walls/${group}/clear`, { transitionMs }),
  endpoints: () => call<EndpointsInfo>("GET", "/api/endpoints"),
  createEndpoint: (e: Partial<Endpoint>) => call<EndpointView>("POST", "/api/endpoints", e),
  putEndpoint: (id: string, e: Partial<Endpoint>) => call<EndpointView>("PUT", `/api/endpoints/${id}`, e),
  deleteEndpoint: (id: string) => call("DELETE", `/api/endpoints/${id}`),
  saveScene: (name: string) => call<Scene>("POST", "/api/scenes", { name }),
  recallScene: (id: string, transitionMs: number) => call("POST", `/api/scenes/${id}/recall`, { transitionMs }),
  deleteScene: (id: string) => call("DELETE", `/api/scenes/${id}`),
};
