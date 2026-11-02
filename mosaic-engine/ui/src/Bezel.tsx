import { useEffect, useMemo, useRef, useState } from "react";
import { api, storedKey, type Layout, type Output, type Rect } from "./api";
import { toast } from "./Dialogs";

const PATTERN_NAME = "Alignment (temporary)";

// The selected displays arranged as rows and columns (by where their centres are).
function detectGrid(outs: Output[]) {
  const byY = [...outs].sort((a, b) => a.crop.y + a.crop.h / 2 - (b.crop.y + b.crop.h / 2));
  const rows: Output[][] = [];
  for (const o of byY) {
    const cy = o.crop.y + o.crop.h / 2;
    const row = rows[rows.length - 1];
    const ref = row?.[0];
    if (ref && Math.abs(cy - (ref.crop.y + ref.crop.h / 2)) < ref.crop.h / 2) row.push(o);
    else rows.push([o]);
  }
  rows.forEach((r) => r.sort((a, b) => a.crop.x - b.crop.x));
  const cols = Math.max(...rows.map((r) => r.length));
  const regular = rows.every((r) => r.length === cols);
  const colW = Array.from({ length: cols }, (_, c) => Math.max(...rows.map((r) => r[c]?.crop.w ?? 0)));
  const rowH = rows.map((r) => Math.max(...r.map((o) => o.crop.h)));
  const anchor = { x: Math.min(...outs.map((o) => o.crop.x)), y: Math.min(...outs.map((o) => o.crop.y)) };
  // The gaps as they are now: average distance between neighbours.
  const gx: number[] = [];
  const gy: number[] = [];
  rows.forEach((r) => r.forEach((o, c) => c > 0 && gx.push(o.crop.x - (r[c - 1].crop.x + r[c - 1].crop.w))));
  rows.forEach((r, i) => i > 0 && r[0] && rows[i - 1][0] && gy.push(r[0].crop.y - (rows[i - 1][0].crop.y + rows[i - 1][0].crop.h)));
  const avg = (v: number[]) => (v.length ? Math.max(0, Math.round(v.reduce((a, b) => a + b, 0) / v.length)) : 0);
  return { rows, cols, regular, colW, rowH, anchor, gapX: avg(gx), gapY: avg(gy) };
}

// Bezel correction for any set of displays in a grid (a 2 x 2 of TVs, say): set the gap the
// bezels make, and every display's region moves so pictures continue straight across them.
export function BezelPanel({ layout, ids, onClose, onError }: { layout: Layout; ids: string[]; onClose: () => void; onError: (e: unknown) => void }) {
  // Sizes and positions are taken once, when the panel opens; the gaps are what changes.
  const start = useMemo(() => layout.outputs.filter((o) => ids.includes(o.id) && o.source === "canvas"), [ids.join(",")]); // eslint-disable-line react-hooks/exhaustive-deps
  const grid = useMemo(() => detectGrid(start), [start]);
  const [gap, setGap] = useState({ x: grid.gapX, y: grid.gapY });
  const [unit, setUnit] = useState<"px" | "mm">("px");
  const [pictureMm, setPictureMm] = useState(1210); // picture width of one display, for mm
  const [pattern, setPattern] = useState<string | null>(null); // id of the temporary alignment source
  const patternRef = useRef<string | null>(null);
  patternRef.current = pattern;

  // A wall group whose displays are exactly these: its own gap settings are updated instead, so they stay in sync.
  const group = layout.walls.find((w) => {
    const members = layout.outputs.filter((o) => o.group === w.id).map((o) => o.id);
    return members.length === start.length && members.every((m) => ids.includes(m)) && w.cols === grid.cols && w.rows === grid.rows.length;
  });
  const tilted = start.some((o) => o.angle);
  const pxPerMm = grid.colW[0] / Math.max(1, pictureMm);

  const cropsFor = (gx: number, gy: number) => {
    const crops: Record<string, Rect> = {};
    let y = grid.anchor.y;
    grid.rows.forEach((row, r) => {
      let x = grid.anchor.x;
      row.forEach((o, c) => {
        crops[o.id] = { ...o.crop, x: Math.round(x), y: Math.round(y) };
        x += grid.colW[c] + gx;
      });
      y += grid.rowH[r] + gy;
    });
    return crops;
  };
  const boundsOf = (crops: Record<string, Rect>): Rect => {
    const rs = Object.values(crops);
    const x = Math.min(...rs.map((r) => r.x)), y = Math.min(...rs.map((r) => r.y));
    return { x, y, w: Math.max(...rs.map((r) => r.x + r.w)) - x, h: Math.max(...rs.map((r) => r.y + r.h)) - y };
  };

  // Changes go out at most every 120 ms while a slider moves; the last one always does.
  const timer = useRef<number | undefined>(undefined);
  const latest = useRef(gap);
  const send = (g: { x: number; y: number }) => {
    const crops = cropsFor(g.x, g.y);
    const jobs: Promise<unknown>[] = [];
    if (group && !tilted) jobs.push(api.putWall(group.id, { gapX: g.x, gapY: g.y }));
    else {
      // Gaps make the displays reach further: grow the canvas if they'd run off it (sources that
      // filled the old canvas keep filling it). Read fresh, so a layer added meanwhile isn't lost.
      jobs.push(
        api.layout().then((cur) => {
          const b = boundsOf(crops);
          const { width: ow, height: oh } = cur.canvas;
          const width = Math.min(16384, Math.max(ow, b.x + b.w));
          const height = Math.min(16384, Math.max(oh, b.y + b.h));
          const grow = width !== ow || height !== oh;
          return api.putLayout({
            outputs: cur.outputs.map((o) => (crops[o.id] ? { ...o, crop: crops[o.id] } : o)),
            ...(grow && {
              canvas: { ...cur.canvas, width, height },
              sources: cur.sources.map((x) => (x.rect.x === 0 && x.rect.y === 0 && x.rect.w === ow && x.rect.h === oh ? { ...x, rect: { x: 0, y: 0, w: width, h: height } } : x)),
            }),
          });
        }),
      );
    }
    if (patternRef.current) jobs.push(api.patchSource(patternRef.current, { rect: boundsOf(crops) }));
    return Promise.all(jobs).catch(onError);
  };
  const update = (g: { x: number; y: number }) => {
    const clamped = { x: Math.max(0, Math.round(g.x)), y: Math.max(0, Math.round(g.y)) };
    setGap(clamped);
    latest.current = clamped;
    window.clearTimeout(timer.current);
    timer.current = window.setTimeout(() => send(latest.current), 120);
  };

  const togglePattern = async () => {
    if (pattern) {
      await api.deleteSource(pattern).catch(onError);
      setPattern(null);
      return;
    }
    // A frozen test pattern over the whole selection, on top: its circle and grid lines show
    // straight away whether they continue across the bezels.
    const s = await api
      .addSource({ name: PATTERN_NAME, type: "test_pattern", animated: false, rect: boundsOf(cropsFor(gap.x, gap.y)) })
      .catch(onError);
    if (s) setPattern(s.id);
  };
  // Closing the panel (or the page) removes the alignment pattern; one left behind by a page that
  // closed before it could is removed when the panel next opens.
  useEffect(() => {
    layout.sources.filter((x) => x.name === PATTERN_NAME).forEach((x) => api.deleteSource(x.id).catch(() => {}));
    const onHide = () => {
      if (!patternRef.current) return;
      const key = storedKey();
      fetch(`/api/sources/${patternRef.current}`, { method: "DELETE", keepalive: true, body: "{}", headers: key ? { "X-Api-Key": key } : {} });
    };
    window.addEventListener("pagehide", onHide);
    return () => {
      window.removeEventListener("pagehide", onHide);
      if (patternRef.current) api.deleteSource(patternRef.current).catch(() => {});
    };
  }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const field = (axis: "x" | "y", label: string) => {
    const px = gap[axis];
    const shown = unit === "px" ? px : Math.round((px / pxPerMm) * 10) / 10;
    const toPx = (v: number) => (unit === "px" ? v : v * pxPerMm);
    return (
      <div className="bezel-field wide-note">
        <div className="bezel-label">
          <span>{label}</span>
          <span className="bezel-value">
            <button onClick={() => update({ ...gap, [axis]: px - 1 })} title="1 px less">
              −
            </button>
            <input
              type="number"
              step={unit === "px" ? 1 : 0.1}
              value={shown}
              onChange={(e) => update({ ...gap, [axis]: toPx(+e.target.value) })}
            />
            <small>{unit}</small>
            <button onClick={() => update({ ...gap, [axis]: px + 1 })} title="1 px more">
              +
            </button>
          </span>
        </div>
        <input type="range" min={0} max={Math.max(400, px)} value={px} onChange={(e) => update({ ...gap, [axis]: +e.target.value })} />
      </div>
    );
  };

  return (
    <div className="inspector">
      <div className="inspector-head">
        <h2>Bezel correction</h2>
        <button onClick={onClose}>Done</button>
      </div>
      <small className="muted">
        {start.length} displays · {grid.regular ? `${grid.cols} × ${grid.rows.length} grid` : "uneven rows"}
        {group ? ` · group ${group.name}` : ""}. Set the gap the bezels make: every display's picture moves so lines continue straight
        across the frames.
      </small>
      {!grid.regular && <small className="warn">The rows have different numbers of displays; gaps are applied row by row from the left.</small>}

      <div className="group">
        <div className="seg">
          {(["px", "mm"] as const).map((u) => (
            <button key={u} className={unit === u ? "on" : ""} onClick={() => setUnit(u)}>
              {u === "px" ? "Pixels" : "Millimetres"}
            </button>
          ))}
        </div>
        {unit === "mm" && (
          <label className="field wide">
            <span>Picture width of one display (mm), from edge to edge of the lit area</span>
            <input type="number" value={pictureMm} onChange={(e) => setPictureMm(Math.max(1, +e.target.value))} />
          </label>
        )}
        {field("x", "Between columns")}
        {field("y", "Between rows")}
        {unit === "mm" && (
          <small className="muted wide-note">
            Measure from the edge of one picture to the edge of the next: both bezels plus any gap. {grid.colW[0]} px ÷ {pictureMm} mm ={" "}
            {pxPerMm.toFixed(3)} px/mm.
          </small>
        )}
      </div>

      <div className="group">
        <h3>Check it on the wall</h3>
        <button className={pattern ? "primary" : ""} onClick={togglePattern}>
          {pattern ? "Hide alignment pattern" : "Show alignment pattern"}
        </button>
        <small className="muted wide-note">
          Shows a test pattern over these displays only. Adjust until its circle and lines run straight across the bezels.
        </small>
      </div>

      <div className="row-buttons">
        <button
          onClick={() => {
            update({ x: grid.gapX, y: grid.gapY });
            toast("Back to how it was");
          }}
        >
          Reset
        </button>
        <button onClick={() => update({ x: 0, y: 0 })}>No gap</button>
      </div>
      {tilted && <small className="warn">Some of these displays are tilted; their tilt stays as it is.</small>}
      <small className="muted">Drag any of the selected displays to move them all. Ctrl+Z undoes. Esc ends the selection.</small>
    </div>
  );
}
