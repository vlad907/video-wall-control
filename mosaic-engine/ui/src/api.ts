// Typed client for the engine's REST API.

export type Rect = { x: number; y: number; w: number; h: number };
export type ScaleMode = "stretch" | "fit" | "fill";
export type SourceType = "test_pattern" | "ndi" | "solid" | "image" | "video" | "capture";

// A sound output of the media server: an NDI source's audio on one of its sound devices.
export interface AudioOutput {
  id: string;
  name: string;
  device: string; // Windows sound device id ("" = default)
  deviceName?: string;
  source: string; // NDI source name
  channelPair: number; // 0 = channels 1-2, 1 = 3-4…
  volumeDb: number;
  mute: boolean;
  matchWall: boolean; // delay = the wall's delay + delayMs (a trim)
  delayMs: number;
  enabled: boolean;
}

export interface AudioInfo {
  wallDelayMs: number;
  devices: { id: string; name: string; channels: number; rate: number; default: boolean }[];
  devicesError: string;
  inputs?: { id: string; name: string; channels: number; rate: number }[]; // sound inputs (capture card audio): source "input:<id>"
  inputsError?: string;
  ndiSources: string[];
  outputs: {
    id: string;
    device: string;
    channels: number;
    error: string;
    sourceConnected: boolean;
    sourceError: string;
    levelDb: number[]; // left, right peak since the last read (dBFS; -100 = silence)
    delayMs: number;
    offByMs: number;
    jumps: number; // times it had to skip (a glitch)
    silentMs: number;
  }[];
}

// A receiving PC's GPU (NVIDIA only) and what Windows logged about its display stack.
export interface Diagnostics {
  gpu?: {
    error?: string;
    util?: number; // % now
    utilMax60?: number; // % highest in the last minute
    utilAvg60?: number;
    memUsedMB?: number;
    memTotalMB?: number;
    tempC?: number;
    tempMax60?: number;
    clockMHz?: number;
    pstate?: number;
    throttle60?: string[]; // why the GPU was slowed down in the last minute (power cap, thermal…)
  } | null;
  driverEvents?: { at: string; source: string; id: number; level: string; message: string }[]; // last hour, newest first
  driverEventsError?: string | null;
  screenChanges?: { at: string; change: string }[]; // last hour
}

// A moment the receiver's screens kept the same picture, and what held it up:
// network / wait / upload / present / screen / loop (see docs/RECEIVER.md).
export interface Freeze {
  at: string; // local time on the receiving PC
  ms: number;
  cause: string;
  detail: string;
}

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
  hdr?: boolean;
  bitDepth?: number;
  decoder?: string; // how it decodes, and why not on the GPU if it doesn't
}

export interface Source {
  id: string;
  name: string;
  type: SourceType;
  ndiName?: string;
  image?: string; // media file name (type "image")
  video?: string; // media file name (type "video")
  device?: string; // capture device name (type "capture")
  loop?: boolean; // video: start again at the end
  playing?: boolean; // video: false = paused
  rect: Rect;
  crop: Rect; // region of the source drawn; w/h 0 = all of it
  opacity: number;
  color?: [number, number, number, number];
  animated?: boolean;
  enabled: boolean;
  status?: { status: string; inputFps?: number; sentFps?: number; framesDropped?: number; width: number; height: number; signal?: "ok" | "lost"; showing?: string; video?: VideoStatus };
  group?: string; // wall group whose content this is (set by the group's presets)
}

// What one wall group shows; source rects are relative to the group's top-left corner.
export interface GroupPreset {
  id: string;
  name: string;
  sources: Source[];
}

// Colour correction of one output, to match displays to each other.
export interface ColorAdjust {
  brightness: number; // -0.5..0.5
  contrast: number; // 0..3
  saturation: number; // 0..3
  gamma: number; // 0.2..5
  red: number; // 0..2 gains
  green: number;
  blue: number;
}
export const NEUTRAL_COLOR: ColorAdjust = { brightness: 0, contrast: 1, saturation: 1, gamma: 1, red: 1, green: 1, blue: 1 };

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
  color?: ColorAdjust; // colour correction (absent = neutral)
  display?: string; // a monitor on the canvas's GPU ("\\\\.\\DISPLAY3"): sent there full screen instead of as NDI
  displayMode?: DisplayMode | null; // display outputs: the mode the monitor is put in (absent = as Windows has it)
  delayMs?: number; // display outputs: picture held back this long (0..1000), to line up with NDI walls
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
  // The screen shows one cell of the output's picture cut into cols x rows (index 1..N, row by
  // row): a 2 x 2 of screens fed by one stream. Absent = the whole picture. Mosaic Receiver only.
  // gapX / gapY: bezel correction, the gap between neighbouring pictures in pixels of the stream;
  // that much of the picture is left out between the cells.
  part?: { cols: number; rows: number; index: number; gapX?: number; gapY?: number };
}

export interface Endpoint {
  id: string;
  name: string;
  host: string; // the PC's name
  player?: "studio_monitor" | "mosaic"; // what shows the streams on that PC
  flashTest?: boolean; // Mosaic Receiver: flash all displays white together (film it to see the offset)
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
  identifySecondsLeft?: number; // its screens show their numbers for this much longer (0 = off)
  receiverVersion?: string; // the receiver this PC installed (from the engine)
  receiverUpdate?: string;  // why it couldn't update or switch, if so
  appVersion?: string; // the dashboard's version (git commit)
  diagnostics?: Diagnostics; // the PC's GPU and display-driver health, from its dashboard
  receiver?: {
    spreadMax10s: number;
    alignedRatio: number;
    refreshHz: number;
    presenter: string;
    error: string;
    presentFps?: number;
    uploadMs?: number;
    timing?: string;
    screenSpreadMs?: number;
    screenSpreadMaxMs?: number;
    screenSpreadNowMs?: number; // the same, last ~2 s
    compensate?: boolean;
    bufferMs?: number; // the buffer it runs with
    bufferReady?: boolean; // found the engine's clock: frames are shown on schedule
    heldMs?: number; // time from a frame arriving to it going on screen
    gapsAbsorbed?: number; // network stalls the buffer hid
    freezesLastMinute?: number; // times the screens kept the same picture for a while
    freezesTotal?: number;
    freezes?: Freeze[]; // the most recent, newest first
  };
  displays: (EndpointDisplay & {
    shownFrameId?: number;
    late?: number;
    fps?: number; // new pictures per second it really shows
    offsetMs?: number; // on screen this much later than the first display, on average (last 10 s)
    offsetMaxMs?: number;
    missedRefreshes?: number; // refreshes that repeated a picture because the receiver fell behind
    windowsLateMs?: number; // how much later Windows itself shows this display than the soonest one
    heldRefreshes?: number; // refreshes the receiver holds this display back to match the others
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

export interface UpdateInfo {
  server: { host: string; version: string; canUpdate: boolean; lastUpdate: { time: number; ok: boolean; result: string } | null };
  receiverVersion: string;
  request: { id?: number; requestedAt?: number; target?: "all" | "receivers" | "server"; serverError?: string };
  endpoints: { host: string; name: string; online: boolean; version: string; receiverVersion: string; update?: { id?: number; state?: "updating" | "updated" | "failed"; message?: string; time?: string } }[];
  now: number;
}

export interface EndpointsInfo {
  endpoints: EndpointView[];
  unassigned: { host: string; address: string; screens: Screen[]; online: boolean }[];
  elsewhere?: { host: string; name: string; layout: string }[]; // PCs set up on other canvases
  // How far apart this canvas's receiving PCs show the same frames (Mosaic Receivers only).
  groupSync?: { pcs: number; frames?: number; spreadMs?: number; maxMs?: number; perPc?: { name: string; offsetMs: number }[] };
  engineHost: string;
  receiverVersion?: string; // the receiver the engine hands out
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
  receiverBufferMs?: number; // Mosaic Receivers show frames this long after they're made (hides network stalls)
  gpu?: number; // the media server GPU this canvas renders on (-1 = default)
  gpuName?: string;
  ndiPrefix?: string; // new outputs' NDI names: <ndiPrefix>_01, ...
  audioOutputs?: AudioOutput[]; // the media server's sound outputs
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
  active: boolean; // on the canvas being edited
  running?: boolean; // on any canvas
}

// A canvas: a layout running on its own render thread (several can run at once).
export interface CanvasInfo {
  id: string;
  layout: string;
  name: string;
  primary: boolean;
  current: boolean;
  running: boolean;
  gpu: string; // the GPU it runs on now
  gpuIndex: number;
  gpuWanted: number;
  error: string;
  outputs: number;
  endpoints: number;
  frameRate: number;
  renderFps?: number;
  canvas: { width: number; height: number };
}

// A GPU in the media server and the monitors connected to it.
export interface GpuInfo {
  index: number;
  name: string;
  memoryMB: number;
  outputs: { name: string; x: number; y: number; width: number; height: number; refreshHz: number; primary: boolean; modes?: DisplayMode[] }[];
  canvases: string[];
}

export interface DisplayMode {
  width: number;
  height: number;
  refreshHz: number;
}

// The canvas this editor window works on ("" = the primary). Sent with every request
// (X-Mosaic-Canvas, or ?canvas= where a header can't go); remembered in this browser.
const CANVAS = "mosaic.canvas";
let canvasId = (() => {
  try {
    return localStorage.getItem(CANVAS) ?? "";
  } catch {
    return "";
  }
})();
export const currentCanvas = () => canvasId;
// Switches the editor to another canvas: everything is loaded again for it.
export function switchCanvas(id: string) {
  canvasId = id;
  try {
    if (id) localStorage.setItem(CANVAS, id);
    else localStorage.removeItem(CANVAS);
  } catch {
    /* private window */
  }
  window.location.reload();
}
export const withCanvas = (url: string) => (canvasId ? `${url}${url.includes("?") ? "&" : "?"}canvas=${encodeURIComponent(canvasId)}` : url);

export interface OutputMetrics {
  // Display outputs (sink "display"): the monitor and what reached it (no NDI numbers).
  display?: string;
  open?: boolean;
  error?: string;
  modeError?: string; // the monitor couldn't be put in the mode asked for
  heldFrames?: number; // frames held back for the output's delay
  presents?: number;
  busy?: number;
  repeats?: number;
  refreshHz?: number;
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
  sources: { id: string; type: string; status: string; inputFps?: number; sentFps?: number; framesDropped?: number; width: number; height: number; signal?: "ok" | "lost"; showing?: string; video?: VideoStatus }[];
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
  if (canvasId) headers["X-Mosaic-Canvas"] = canvasId;
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
      if (canvasId) x.setRequestHeader("X-Mosaic-Canvas", canvasId);
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
  canvases: () => call<{ canvases: CanvasInfo[]; current: string; problems: string[] }>("GET", "/api/canvases"),
  addCanvas: (body: { name: string } | { layout: string }) => call<{ id: string; layout: string; problems: string[] }>("POST", "/api/canvases", body),
  stopCanvas: (id: string) => call("DELETE", `/api/canvases/${id}`),
  gpus: () => call<{ gpus: GpuInfo[] }>("GET", "/api/gpus").then((r) => r.gpus),
  capture: () =>
    call<{ devices: { name: string; kind: string }[]; open: { name: string; width: number; height: number; fps: number; viewers: number; error: string }[]; error: string }>(
      "GET",
      "/api/capture",
    ),
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
  identifyEndpoint: (id: string, on: boolean) => call<EndpointView>("POST", `/api/endpoints/${id}/identify`, { on }),
  saveScene: (name: string) => call<Scene>("POST", "/api/scenes", { name }),
  recallScene: (id: string, transitionMs: number) => call("POST", `/api/scenes/${id}/recall`, { transitionMs }),
  deleteScene: (id: string) => call("DELETE", `/api/scenes/${id}`),
  updates: () => call<UpdateInfo>("GET", "/api/system/update"),
  audio: () => call<AudioInfo>("GET", "/api/audio"),
  startUpdate: (target: "all" | "receivers") => call("POST", "/api/system/update", { target }),
};
