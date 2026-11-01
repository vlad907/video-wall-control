// Small stroke icons (24 px grid, drawn at 18 px) for the navigation rail and toolbars.
const paths: Record<string, string> = {
  screens: "M3 4h8v7H3zM13 4h8v7h-8zM3 13h8v7H3zM13 13h8v7h-8z",
  inputs: "M12 3l9 5-9 5-9-5 9-5zM3 13l9 5 9-5M3 17l9 5 9-5",
  presets: "M6 3h12v18l-6-4-6 4z",
  endpoints: "M3 4h18v12H3zM8 20h8M12 16v4",
  monitor: "M3 12h4l3-8 4 16 3-8h4",
  magnet: "M6 3v8a6 6 0 0012 0V3M6 7h4M14 7h4",
  grid: "M3 3h18v18H3zM9 3v18M15 3v18M3 9h18M3 15h18",
  plus: "M12 5v14M5 12h14",
  more: "M5 12h.01M12 12h.01M19 12h.01",
  close: "M6 6l12 12M18 6L6 18",
  play: "M8 5l11 7-11 7z",
};

export function Icon({ name, size = 18 }: { name: keyof typeof paths | string; size?: number }) {
  return (
    <svg width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth={1.8} strokeLinecap="round" strokeLinejoin="round" aria-hidden>
      <path d={paths[name] ?? ""} />
    </svg>
  );
}
