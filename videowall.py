"""
Video Wall Control - background service + web dashboard for a 4-panel
NDI Studio Monitor mosaic. Also starts and supervises the GPU mosaic engine
(mosaic-engine/) when it has been built.

Pure Python standard library (3.8+), no pip install needed.
Run with pythonw.exe on Windows so no console window appears.

    pythonw videowall.py            # normal (background) start
    python  videowall.py --console  # foreground, logs to the console too
"""

import glob
import json
import logging
import logging.handlers
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
CONFIG_PATH = Path(os.environ.get("VIDEOWALL_CONFIG", BASE_DIR / "config.json"))
STATIC_DIR = BASE_DIR / "static"
LOG_DIR = BASE_DIR / "logs"

IS_WINDOWS = os.name == "nt"
CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
CREATE_BREAKAWAY_FROM_JOB = 0x01000000

DEFAULT_CONFIG = {
    "dashboard_host": "0.0.0.0",
    "dashboard_port": 8080,
    "monitor_host": "127.0.0.1",
    "manage_processes": True,
    "exe_name": "Application.Network.VideoMonitor2.x64.exe",
    "exe_path": "",
    "exe_search_paths": [
        r"C:\Program Files\NDI\NDI 6 Tools\Studio Monitor",
        r"C:\Program Files\NDI\NDI 5 Tools\Studio Monitor",
        r"C:\Program Files\NewTek\NDI 5 Tools\Studio Monitor",
        r"C:\Program Files\NewTek\NDI 4 Tools\Studio Monitor",
        r"C:\Program Files\NDI\*\Studio Monitor",
        r"C:\Program Files\NewTek\*\Studio Monitor",
    ],
    "boot_delay_seconds": 10,
    "launch_gap_seconds": 2,
    "api_ready_timeout_seconds": 20,
    "watchdog_interval_seconds": 15,
    "enforce_mapping": True,
    "mdns_discovery": True,
    "fullscreen": True,
    "hung_restart_after_checks": 4,
    "restart_cooldown_seconds": 300,
    "layout_columns": 2,
    "window_options": {"hide_border": True, "always_on_top": True},
    "decorations": {"hw_accel": True},
    # GPU mosaic engine (mosaic-engine/): started before the Studio Monitors and
    # restarted if it stops answering. exe_path empty = the build in mosaic-engine/build.
    # host: the PC that runs the engine; empty = this one. With another PC set, this
    # dashboard doesn't start an engine of its own, it only reads that one's outputs.
    "mosaic": {"enabled": True, "exe_path": "", "port": 8090, "args": [], "host": ""},
    "panels": [
        {"name": "Panel 1", "port": 80, "ndi_source": "", "display_device": ""},
        {"name": "Panel 2", "port": 81, "ndi_source": "", "display_device": ""},
        {"name": "Panel 3", "port": 82, "ndi_source": "", "display_device": ""},
        {"name": "Panel 4", "port": 83, "ndi_source": "", "display_device": ""},
    ],
}

# pythonw.exe has no stdout/stderr; anything writing to them would crash.
if sys.stdout is None:
    sys.stdout = open(os.devnull, "w")
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w")

log = logging.getLogger("videowall")
recent_log = deque(maxlen=300)


class MemoryHandler(logging.Handler):
    def emit(self, record):
        recent_log.append({
            "time": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(record.created)),
            "level": record.levelname,
            "message": record.getMessage(),
        })


def setup_logging(console):
    LOG_DIR.mkdir(exist_ok=True)
    fmt = logging.Formatter("%(asctime)s %(levelname)-7s %(message)s")
    file_handler = logging.handlers.RotatingFileHandler(
        LOG_DIR / "videowall.log", maxBytes=2_000_000, backupCount=3, encoding="utf-8")
    file_handler.setFormatter(fmt)
    log.addHandler(file_handler)
    log.addHandler(MemoryHandler())
    if console:
        stream = logging.StreamHandler()
        stream.setFormatter(fmt)
        log.addHandler(stream)
    log.setLevel(logging.INFO)


# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------

config_lock = threading.RLock()
config = {}


def load_config():
    global config
    data = {}
    if CONFIG_PATH.exists():
        try:
            data = json.loads(CONFIG_PATH.read_text(encoding="utf-8-sig"))
        except (OSError, ValueError) as exc:
            log.error("Could not read %s (%s); using defaults", CONFIG_PATH, exc)
    merged = json.loads(json.dumps(DEFAULT_CONFIG))
    merged.update(data)
    with config_lock:
        config = merged
    if not CONFIG_PATH.exists():
        save_config()


def save_config():
    with config_lock:
        text = json.dumps(config, indent=2)
    tmp = CONFIG_PATH.with_suffix(".json.tmp")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, CONFIG_PATH)


def cfg(key):
    with config_lock:
        return config.get(key, DEFAULT_CONFIG.get(key))


def panels():
    with config_lock:
        return [dict(p) for p in config["panels"]]


def mosaic_cfg():
    with config_lock:
        merged = dict(DEFAULT_CONFIG["mosaic"])
        merged.update(config.get("mosaic") or {})
        return merged


# --------------------------------------------------------------------------
# NDI Studio Monitor HTTP API
# --------------------------------------------------------------------------

def monitor_url(port, path):
    return "http://%s:%d%s" % (cfg("monitor_host"), port, path)


def http_json(method, url, body=None, timeout=3):
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8", "replace").strip()
    if not raw:
        return None
    try:
        return json.loads(raw)
    except ValueError:
        return raw


def get_configuration(port):
    """Return the panel's current configuration dict, or None if unreachable."""
    try:
        result = http_json("GET", monitor_url(port, "/v1/configuration"))
        return result if isinstance(result, dict) else {}
    except (urllib.error.URLError, OSError, ValueError):
        return None


NDI_NAME = re.compile(r"^[^()]+ \(.+\)$")  # NDI names look like "MACHINE (Stream)"


def get_raw_sources():
    """Return (port, raw /v1/sources response) from the first reachable panel."""
    last_error = None
    for p in panels():
        try:
            return p["port"], http_json("GET", monitor_url(p["port"], "/v1/sources"), timeout=5)
        except (urllib.error.URLError, OSError, ValueError) as exc:
            last_error = exc
    raise RuntimeError("No Studio Monitor instance reachable (%s)" % last_error)


def get_sources():
    """Return a sorted list of NDI source names, from Studio Monitor and/or mDNS."""
    names, error = set(), None
    try:
        _, result = get_raw_sources()
        names |= set(normalize_sources(result)) | set(find_ndi_names(result))
    except RuntimeError as exc:
        error = exc
    if cfg("mdns_discovery"):
        names |= mdns_sources()
    if not names and error:
        raise error
    return sorted(names, key=str.lower)


# --------------------------------------------------------------------------
# NDI discovery via mDNS (works even when no Studio Monitor is running)
# --------------------------------------------------------------------------

NDI_SERVICE = "_ndi._tcp.local"
mdns_lock = threading.Lock()
mdns_seen = {}   # name -> last time seen
mdns_state = {"last_query": 0.0}


def local_ipv4_addresses():
    addrs = set()
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            addrs.add(info[4][0])
    except OSError:
        pass
    return [a for a in addrs if not a.startswith("127.")]


def dns_read_name(data, offset):
    labels, jumped, end = [], False, offset
    for _ in range(128):
        length = data[offset]
        if length == 0:
            offset += 1
            break
        if length & 0xC0 == 0xC0:
            if not jumped:
                end = offset + 2
            offset = ((length & 0x3F) << 8) | data[offset + 1]
            jumped = True
            continue
        labels.append(data[offset + 1:offset + 1 + length].decode("utf-8", "replace"))
        offset += 1 + length
    return ".".join(labels), (end if jumped else offset)


def dns_ptr_targets(data, service):
    """Return PTR targets for `service` found anywhere in a DNS response packet."""
    qd, an, ns, ar = struct.unpack("!4H", data[4:12])
    offset = 12
    for _ in range(qd):
        _, offset = dns_read_name(data, offset)
        offset += 4
    targets = []
    for _ in range(an + ns + ar):
        name, offset = dns_read_name(data, offset)
        rtype, _, _, rdlength = struct.unpack("!HHIH", data[offset:offset + 10])
        offset += 10
        if rtype == 12 and name.lower().rstrip(".") == service:
            target, _ = dns_read_name(data, offset)
            targets.append(target)
        offset += rdlength
    return targets


def mdns_query_packet(service):
    qname = b"".join(bytes([len(p)]) + p.encode() for p in service.split(".")) + b"\0"
    # PTR question, class IN with the "unicast response" bit set.
    return struct.pack("!6H", 0, 0, 1, 0, 0, 0) + qname + struct.pack("!HH", 12, 0x8001)


def mdns_browse(timeout=1.5):
    found = set()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    try:
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
        sock.bind(("", 0))
        sock.settimeout(0.25)
        packet = mdns_query_packet(NDI_SERVICE)
        interfaces = local_ipv4_addresses() or [None]
        for attempt in range(2):
            for iface in interfaces:
                try:
                    if iface:
                        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(iface))
                    sock.sendto(packet, ("224.0.0.251", 5353))
                except OSError:
                    pass
            deadline = time.time() + timeout / 2
            while time.time() < deadline:
                try:
                    data, _ = sock.recvfrom(9000)
                except socket.timeout:
                    continue
                except OSError:
                    break
                try:
                    for target in dns_ptr_targets(data, NDI_SERVICE):
                        suffix = "." + NDI_SERVICE
                        if target.lower().endswith(suffix):
                            found.add(target[:-len(suffix)])
                except (IndexError, struct.error):
                    continue
    finally:
        sock.close()
    return found


def mdns_sources():
    """NDI names seen via mDNS in the last minute (queries at most every 5 s)."""
    with mdns_lock:
        now = time.time()
        if now - mdns_state["last_query"] > 5:
            mdns_state["last_query"] = now
            try:
                for name in mdns_browse():
                    mdns_seen[name] = now
            except OSError as exc:
                log.debug("mDNS browse failed: %s", exc)
        for name, seen in list(mdns_seen.items()):
            if now - seen > 60:
                del mdns_seen[name]
        return set(mdns_seen)


def find_ndi_names(obj):
    """Collect every string shaped like an NDI name anywhere in the JSON (keys or values)."""
    if isinstance(obj, str):
        return [obj] if NDI_NAME.match(obj.strip()) else []
    found = []
    if isinstance(obj, dict):
        for key, value in obj.items():
            found += find_ndi_names(key) + find_ndi_names(value)
    elif isinstance(obj, list):
        for item in obj:
            found += find_ndi_names(item)
    return [s.strip() for s in found]


def normalize_sources(result):
    if isinstance(result, dict):
        for key in ("sources", "NDI_sources", "items", "data"):
            if key in result:
                return normalize_sources(result[key])
        return []
    names = []
    if isinstance(result, list):
        for item in result:
            if isinstance(item, str):
                names.append(item)
            elif isinstance(item, dict):
                for key in ("name", "NDI_source", "source", "ndi_name"):
                    if item.get(key):
                        names.append(str(item[key]))
                        break
    return names


def window_rect(window):
    """(left, top, right, bottom) of Studio Monitor's normal window position, or None."""
    try:
        rect = tuple(int(window[k]) for k in ("normal_posn_left", "normal_posn_top",
                                               "normal_posn_right", "normal_posn_bottom"))
    except (KeyError, TypeError, ValueError):
        return None
    return rect if rect[2] > rect[0] and rect[3] > rect[1] else None


def display_rect(d):
    return (d["x"], d["y"], d["x"] + d["width"], d["y"] + d["height"])


def overlap_area(a, b):
    w = min(a[2], b[2]) - max(a[0], b[0])
    h = min(a[3], b[3]) - max(a[1], b[1])
    return max(w, 0) * max(h, 0)


def resolve_screen(panel, current, displays):
    """Pick the display this panel fills and remember it in config.json.

    Uses the saved screen if it still exists; otherwise the screen the window is
    mostly on now, skipping screens already claimed by other panels.
    """
    by_device = {d["device"]: d for d in displays}
    if panel.get("screen") in by_device:
        return by_device[panel["screen"]]
    taken = {p.get("screen") for p in panels() if p["port"] != panel["port"]}
    free = [d for d in displays if d["device"] not in taken] or displays
    if len(displays) > len(panels()):
        # More screens than panels: the primary is likely the control screen, try it last.
        free = sorted(free, key=lambda d: d.get("primary", False))
    rect = window_rect((current or {}).get("window") or {})
    best = None
    if rect:
        best = max(free, key=lambda d: overlap_area(rect, display_rect(d)))
        if overlap_area(rect, display_rect(best)) == 0:
            best = None
    best = best or free[0]
    with config_lock:
        for p in config["panels"]:
            if p["port"] == panel["port"]:
                p["screen"] = best["device"]
    save_config()
    log.info("%s assigned to screen %s (%dx%d at %d,%d)", panel["name"], best["short"],
             best["width"], best["height"], best["x"], best["y"])
    return best


def build_payload(panel, current=None):
    """Start from the panel's current configuration and change only our fields."""
    payload = json.loads(json.dumps(current)) if current else {}
    payload["version"] = payload.get("version", 1)
    payload["NDI_source"] = panel["ndi_source"]
    window = payload.setdefault("window", {})
    window.update(cfg("window_options"))
    if panel.get("display_device"):
        window["display_device"] = panel["display_device"]
    payload.setdefault("decorations", {}).update(cfg("decorations"))
    if cfg("fullscreen"):
        displays = enumerate_displays()
        if displays:
            # Borderless + always on top, sized exactly to the screen = full screen.
            left, top, right, bottom = display_rect(resolve_screen(panel, current, displays))
            window.update(showcmd=1, normal_posn_left=left, normal_posn_top=top,
                          normal_posn_right=right, normal_posn_bottom=bottom)
    return payload


def apply_panel(panel):
    """POST the saved mapping to one Studio Monitor. Returns (ok, message)."""
    current = get_configuration(panel["port"])
    if not panel.get("ndi_source"):
        # No saved source: keep what it's playing but still apply window settings.
        if not (current or {}).get("NDI_source"):
            return False, "%s: no NDI source assigned, skipped" % panel["name"]
        panel = dict(panel, ndi_source=current["NDI_source"])
    try:
        payload = build_payload(panel, current)
        http_json("POST", monitor_url(panel["port"], "/v1/configuration"), payload, timeout=5)
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")[:200]
        log.error("%s (port %d): HTTP %d %s", panel["name"], panel["port"], exc.code, detail)
        return False, "%s: HTTP %d %s" % (panel["name"], exc.code, detail)
    except (urllib.error.URLError, OSError) as exc:
        log.error("%s (port %d): apply failed: %s", panel["name"], panel["port"], exc)
        return False, "%s: %s" % (panel["name"], exc)
    window = payload["window"]
    where = (" full screen %d,%d-%d,%d" % (window["normal_posn_left"], window["normal_posn_top"],
                                            window["normal_posn_right"], window["normal_posn_bottom"])
             if "normal_posn_left" in window and cfg("fullscreen") else "")
    log.info("%s (port %d) -> '%s'%s", panel["name"], panel["port"], panel["ndi_source"], where)
    return True, "%s: applied" % panel["name"]


def apply_all():
    return [apply_panel(p) for p in panels()]


def wait_for_api(ports, timeout):
    deadline = time.time() + timeout
    pending = set(ports)
    while pending and time.time() < deadline:
        for port in list(pending):
            if get_configuration(port) is not None:
                pending.discard(port)
        if pending:
            time.sleep(1)
    if pending:
        log.warning("API not responding on port(s) %s after %ds", sorted(pending), timeout)
    return not pending


# --------------------------------------------------------------------------
# Process management (Windows)
# --------------------------------------------------------------------------

EXE_PATTERN = re.compile(r"(videomonitor|studio ?monitor).*\.exe$", re.I)
exe_cache = {"path": None, "checked": 0.0, "warned": 0.0}


def search_roots():
    roots = []
    for var in ("ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"):
        base = os.environ.get(var)
        if base:
            for vendor in ("NDI", "NewTek", "Vizrt"):
                folder = os.path.join(base, vendor)
                if folder not in roots:
                    roots.append(folder)
    return roots


def scan_for_exe():
    """Walk the NDI/NewTek install folders for a Studio Monitor executable."""
    preferred, fallback = [], []
    for root in search_roots():
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            if dirpath[len(root):].count(os.sep) >= 4:
                dirnames[:] = []
            for f in filenames:
                if f.lower() == cfg("exe_name").lower():
                    preferred.append(os.path.join(dirpath, f))
                elif EXE_PATTERN.search(f) and "uninst" not in f.lower():
                    fallback.append(os.path.join(dirpath, f))
    # Highest NDI version folder first (NDI 6 before NDI 5).
    for found in (preferred, fallback):
        if found:
            return sorted(found, reverse=True)[0]
    return None


def exe_from_start_menu():
    """Resolve NDI 'Studio Monitor' Start Menu shortcuts to their target .exe."""
    if not IS_WINDOWS:
        return None
    links = []
    for var in ("ProgramData", "APPDATA"):
        root = os.path.join(os.environ.get(var, ""), r"Microsoft\Windows\Start Menu\Programs")
        if not os.path.isdir(root):
            continue
        for dirpath, _, filenames in os.walk(root):
            for f in filenames:
                full = os.path.join(dirpath, f)
                if f.lower().endswith(".lnk") and "monitor" in f.lower() and "ndi" in full.lower():
                    links.append(full)
    if not links:
        return None
    script = "$s = New-Object -ComObject WScript.Shell; " + "; ".join(
        "$s.CreateShortcut('%s').TargetPath" % link.replace("'", "''") for link in links)
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                             capture_output=True, text=True, timeout=20,
                             creationflags=CREATE_NO_WINDOW).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        log.warning("Could not read Start Menu shortcuts: %s", exc)
        return None
    targets = [t.strip() for t in out.splitlines()
               if t.strip().lower().endswith(".exe") and os.path.isfile(t.strip())]
    targets.sort(key=lambda t: (not EXE_PATTERN.search(os.path.basename(t)), t))
    return targets[0] if targets else None


def find_exe():
    explicit = cfg("exe_path")
    if explicit:
        if os.path.isfile(explicit):
            return explicit
        log.warning("exe_path '%s' does not exist; searching instead", explicit)
    cached = exe_cache["path"]
    if cached and os.path.isfile(cached):
        return cached
    if time.time() - exe_cache["checked"] < 60:
        return None
    exe_cache["checked"] = time.time()
    path = None
    for folder in cfg("exe_search_paths"):
        for match in sorted(glob.glob(os.path.join(folder, cfg("exe_name"))), reverse=True):
            if os.path.isfile(match):
                path = match
                break
        if path:
            break
    path = path or scan_for_exe() or exe_from_start_menu()
    if path:
        log.info("Using Studio Monitor at %s", path)
    exe_cache["path"] = path
    return path


def process_name():
    """Image name to count/kill: the discovered exe's file name, else the configured one."""
    exe = find_exe()
    return os.path.basename(exe) if exe else cfg("exe_name")


def count_processes():
    if not IS_WINDOWS:
        return None
    name = process_name()
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq %s" % name, "/FO", "CSV", "/NH"],
            capture_output=True, text=True, timeout=10, creationflags=CREATE_NO_WINDOW,
        ).stdout
    except (OSError, subprocess.SubprocessError) as exc:
        log.error("tasklist failed: %s", exc)
        return None
    target = name.lower()
    return sum(1 for line in out.splitlines() if line.strip().strip('"').lower().startswith(target))


def kill_all_instances():
    if not IS_WINDOWS:
        return
    subprocess.run(["taskkill", "/F", "/IM", process_name()],
                   capture_output=True, timeout=15, creationflags=CREATE_NO_WINDOW)
    log.info("Killed all Studio Monitor instances")


def launch_instance(exe):
    kwargs = dict(cwd=os.path.dirname(exe), stdin=subprocess.DEVNULL,
                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
    base_flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    try:
        # Break away from Task Scheduler's job so monitors survive a service restart.
        subprocess.Popen([exe], creationflags=base_flags | CREATE_BREAKAWAY_FROM_JOB, **kwargs)
    except OSError:
        subprocess.Popen([exe], creationflags=base_flags, **kwargs)


def exe_or_warn():
    exe = find_exe()
    # Logged at most every 5 minutes so the watchdog doesn't flood the log.
    if not exe and time.time() - exe_cache["warned"] > 300:
        exe_cache["warned"] = time.time()
        log.error("Studio Monitor executable not found under %s. "
                  "Set its full path in the dashboard.", ", ".join(search_roots()) or "Program Files")
    return exe


def launch_instances(count):
    exe = exe_or_warn()
    if not exe:
        return False
    gap = float(cfg("launch_gap_seconds"))
    for i in range(count):
        log.info("Launching Studio Monitor instance %d/%d", i + 1, count)
        try:
            launch_instance(exe)
        except OSError as exc:
            log.error("Failed to launch %s: %s", exe, exc)
            return False
        time.sleep(gap)
    return True


# --------------------------------------------------------------------------
# GPU mosaic engine (mosaic-engine/)
# --------------------------------------------------------------------------

MOSAIC_DIR = BASE_DIR / "mosaic-engine"
mosaic_state = {"proc": None, "last_start": 0.0, "down_streak": 0, "warned": 0.0}


def find_mosaic_exe():
    configured = (mosaic_cfg().get("exe_path") or "").strip().strip('"')
    if configured:
        return configured if os.path.isfile(configured) else None
    name = "mosaic_engine.exe" if IS_WINDOWS else "mosaic_engine"
    for rel in ("build/Release", "build", "build/RelWithDebInfo"):
        candidate = MOSAIC_DIR / rel / name
        if candidate.is_file():
            return str(candidate)
    return None


def short_host(name):
    return name.split(".")[0].upper() if name and not name.replace(".", "").isdigit() else name


def mosaic_remote_host():
    """The other PC running the engine, or None when it runs here."""
    host = (mosaic_cfg().get("host") or "").strip()
    if not host or host.lower() in ("localhost", "127.0.0.1", "::1"):
        return None
    if short_host(host) == short_host(socket.gethostname()):
        return None
    return host


def parse_mosaic_host(value):
    """Accepts "PC-NAME", "192.168.1.20", "pc:8090" or "http://pc:8090/"; returns (host, port or None)."""
    value = str(value or "").strip()
    if "://" in value:
        value = value.split("://", 1)[1]
    value = value.split("/", 1)[0]
    port = None
    if value.count(":") == 1:
        value, port_text = value.split(":")
        if not port_text.isdigit():
            raise ValueError("Bad port in '%s'" % port_text)
        port = int(port_text)
    if any(c.isspace() for c in value):
        raise ValueError("Bad engine host '%s'" % value)
    return value, port


def mosaic_url(path):
    return "http://%s:%d%s" % (mosaic_remote_host() or "127.0.0.1", int(mosaic_cfg()["port"]), path)


def mosaic_get(path, timeout=1.5):
    try:
        return http_json("GET", mosaic_url(path), timeout=timeout)
    except (urllib.error.URLError, OSError, ValueError):
        return None


def mosaic_alive():
    result = mosaic_get("/api/health")
    return isinstance(result, dict) and result.get("ok") is True


def mosaic_root(exe):
    # The engine finds layouts/, data/ and ui/dist relative to its working directory.
    exe_dir = Path(exe).resolve().parent
    for d in (exe_dir, *exe_dir.parents):
        if (d / "layouts").is_dir() and (d / "ui").is_dir():
            return d
    return MOSAIC_DIR if MOSAIC_DIR.is_dir() else exe_dir


def start_mosaic():
    """Starts the engine unless one is already answering on its port. Returns True when it's up."""
    mc = mosaic_cfg()
    if not mc.get("enabled") or mosaic_remote_host():
        return False
    if mosaic_alive():
        return True
    exe = find_mosaic_exe()
    if not exe:
        if time.time() - mosaic_state["warned"] > 300:
            mosaic_state["warned"] = time.time()
            log.warning("Mosaic engine not built yet (looked in %s); skipping it", MOSAIC_DIR / "build")
        return False
    stop_mosaic()
    LOG_DIR.mkdir(exist_ok=True)
    log_path = LOG_DIR / "mosaic-engine.log"
    if log_path.exists() and log_path.stat().st_size > 5_000_000:
        os.replace(log_path, log_path.with_suffix(".log.1"))
    out = open(log_path, "ab")
    args = [exe, "--port", str(int(mc["port"]))] + [str(a) for a in mc.get("args") or []]
    kwargs = dict(cwd=str(mosaic_root(exe)), stdin=subprocess.DEVNULL, stdout=out, stderr=out, close_fds=True)
    log.info("Starting mosaic engine: %s", " ".join(args))
    mosaic_state["last_start"] = time.time()
    try:
        if IS_WINDOWS:
            flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP
            try:
                # Like the monitors, survive a restart of this service (it re-adopts the engine).
                mosaic_state["proc"] = subprocess.Popen(args, creationflags=flags | CREATE_BREAKAWAY_FROM_JOB, **kwargs)
            except OSError:
                mosaic_state["proc"] = subprocess.Popen(args, creationflags=flags, **kwargs)
        else:
            mosaic_state["proc"] = subprocess.Popen(args, **kwargs)
    except OSError as exc:
        log.error("Could not start the mosaic engine: %s", exc)
        return False
    finally:
        out.close()
    for _ in range(40):
        time.sleep(0.5)
        if mosaic_alive():
            log.info("Mosaic engine is up on port %d", int(mc["port"]))
            mosaic_state["down_streak"] = 0
            return True
        if mosaic_state["proc"].poll() is not None:
            log.error("Mosaic engine exited with code %s (see logs/mosaic-engine.log)", mosaic_state["proc"].returncode)
            return False
    log.error("Mosaic engine did not answer within 20 s (see logs/mosaic-engine.log)")
    return False


def stop_mosaic():
    proc = mosaic_state["proc"]
    mosaic_state["proc"] = None
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
    elif IS_WINDOWS:
        # One we didn't start (e.g. left over from a previous run of this service).
        subprocess.run(["taskkill", "/F", "/IM", "mosaic_engine.exe"],
                       capture_output=True, timeout=15, creationflags=CREATE_NO_WINDOW)


def mosaic_watchdog():
    if not mosaic_cfg().get("enabled") or mosaic_remote_host() or not find_mosaic_exe():
        return
    if mosaic_alive():
        mosaic_state["down_streak"] = 0
        return
    mosaic_state["down_streak"] += 1
    # Two missed checks in a row, and not right after a start: restart it.
    if mosaic_state["down_streak"] >= 2 and time.time() - mosaic_state["last_start"] > 30:
        log.warning("Mosaic engine is not answering; restarting it")
        start_mosaic()


def mosaic_status(host):
    mc = mosaic_cfg()
    remote = mosaic_remote_host()
    metrics = mosaic_get("/api/metrics") if mc.get("enabled") or remote else None
    engine = (metrics or {}).get("engine") or {}
    outputs = [{"id": o.get("id"), "name": o.get("name"), "ndiName": o.get("ndiName"),
                "enabled": o.get("enabled"), "fps": o.get("fps"), "sink": o.get("sink"),
                "connections": o.get("connections")} for o in (metrics or {}).get("outputs") or []]
    return {
        "enabled": bool(mc.get("enabled")),
        "remote": remote,
        "exe": find_mosaic_exe(),
        "running": metrics is not None,
        "editor_url": "http://%s:%d/" % (remote or host, int(mc["port"])),
        "render_fps": engine.get("renderFps"),
        "target_fps": engine.get("targetFps"),
        "gpu": engine.get("device"),
        "ndi": engine.get("ndi"),
        "layout": engine.get("layout"),
        "outputs": outputs,
    }


def mosaic_stream_names():
    """Full NDI names ("MACHINE (Mosaic_Output_01)") of the engine's enabled outputs, in order."""
    remote = mosaic_remote_host()
    result = mosaic_get("/api/outputs", timeout=3)
    if not isinstance(result, dict):
        if remote:
            raise RuntimeError("The mosaic engine on %s is not answering on port %d"
                               % (remote, int(mosaic_cfg()["port"])))
        raise RuntimeError("The mosaic engine is not running")
    outs = [o for o in result.get("outputs") or [] if o.get("enabled", True)]
    try:
        discovered = get_sources()
    except RuntimeError:
        discovered = []
    # NDI names start with the sending PC's name. Prefer the engine's PC; with an IP
    # address for a remote engine, at least skip streams that come from this PC.
    here = short_host(socket.gethostname())
    engine_pc = short_host(remote) if remote else here
    names = []
    for o in outs:
        suffix = "(%s)" % o["ndiName"]
        matches = [n for n in discovered if n.endswith(suffix)]
        exact = [n for n in matches if n.upper().startswith(str(engine_pc).upper() + " ")]
        others = [n for n in matches if not n.upper().startswith(here + " ")] if remote else []
        names.append((exact or others or matches or ["%s %s" % (engine_pc, suffix)])[0])
    return names


def assign_mosaic_to_panels():
    """Points panel 1 at the engine's first output, panel 2 at the second, and so on."""
    names = mosaic_stream_names()
    if not names:
        raise RuntimeError("The mosaic engine has no enabled outputs")
    targets = panels()
    updates = [{"port": p["port"], "ndi_source": n} for p, n in zip(targets, names)]
    update_panels(updates)
    log.info("Assigned mosaic outputs to panels: %s",
             ", ".join("%s -> %s" % (p["name"], n) for p, n in zip(targets, names)))
    with action_lock:
        return [apply_panel(p) for p in panels() if p["port"] in {u["port"] for u in updates}]


# --------------------------------------------------------------------------
# Endpoint mode: the mosaic engine says which output goes on which screen
# --------------------------------------------------------------------------

ENDPOINT_INTERVAL = 5  # seconds between check-ins
endpoint_state = {"endpoint": None, "error": None, "last_ok": None, "displays": [], "receiver_update": ""}


def endpoint_managed():
    return endpoint_state["endpoint"] is not None


def endpoint_report():
    """This PC's screens and what each panel plays, for the engine's endpoint view."""
    report = []
    for i, p in enumerate(panels(), start=1):
        current = get_configuration(p["port"])
        report.append({"index": i, "port": p["port"], "name": p["name"], "screen": p.get("screen") or "",
                       "playing": (current or {}).get("NDI_source") or "", "reachable": current is not None})
    return report


def desired_panels(displays, old, devices):
    """Panel list for the engine's display slots. Unpinned slots keep the screen they got last time.
    A slot pinned to a screen this PC doesn't have is treated as unpinned."""
    base = old[0]["port"] if old else 80
    by_port = {p["port"]: p for p in old}
    pin = lambda d: d.get("screen") if d.get("screen") in devices else ""
    pinned = {pin(d) for d in displays if pin(d)}
    result = []
    for i, d in enumerate(displays):
        port = base + i
        prev = by_port.get(port) or {}
        screen = pin(d)
        if not screen and not prev.get("screen_pinned") and prev.get("screen") not in pinned:
            screen = prev.get("screen") or ""
        panel = {"name": d.get("label") or "Display %d" % (i + 1), "port": port,
                 "ndi_source": d["stream"], "display_device": prev.get("display_device", ""),
                 "screen_pinned": bool(pin(d))}
        if screen:
            panel["screen"] = screen
        result.append(panel)
    return result


def sync_endpoint(displays):
    """Makes the panels match the engine's assignment: count, streams and screens."""
    wanted = [d for d in displays if d.get("stream")]
    devices = {d["device"] for d in enumerate_displays()}
    for d in wanted:
        if d.get("screen") and d["screen"] not in devices and d["screen"] not in endpoint_state.setdefault("warned", set()):
            endpoint_state["warned"].add(d["screen"])
            log.warning("Display %s: this PC has no screen %s; using the next free one", d.get("index"), d["screen"])
    with action_lock:
        old = panels()
        new = desired_panels(wanted, old, devices)
        key = lambda p: (p["port"], p["name"], p["ndi_source"], p.get("screen") or "", p["screen_pinned"])
        if [key(p) for p in new] == [key(dict(p, screen_pinned=p.get("screen_pinned", False))) for p in old]:
            return
        with config_lock:
            config["panels"] = new
        save_config()
        log.info("Endpoint assignment: %s", ", ".join("%s -> %s" % (p["name"], p["ndi_source"]) for p in new) or "no displays")
    if len(new) != len(old):
        # Studio Monitor instances can't be told apart before they answer, so start over.
        if processes_managed():
            restart_all("endpoint now has %d display(s)" % len(new))
        else:
            with action_lock:
                apply_all()
        return
    changed = [p for p, o in zip(new, old) if key(p) != key(dict(o, screen_pinned=o.get("screen_pinned", False)))]
    with action_lock:
        for p in changed:
            apply_panel(p)


# --------------------------------------------------------------------------
# Mosaic Receiver: our own player, used instead of Studio Monitor when the endpoint's
# player is "mosaic". One process shows every display, in step (mosaic-engine/docs/RECEIVER.md).
# --------------------------------------------------------------------------

RECEIVER_PORT = 8095
receiver_state = {"proc": None, "sent": None, "last_start": 0.0, "warned": 0.0, "update_failed": 0.0}


def receiver_mode():
    return cfg("player") == "mosaic"


RECEIVER_DIR = MOSAIC_DIR / ("build/Release" if IS_WINDOWS else "build")


def update_receiver(package):
    """Keeps this PC's Mosaic Receiver the same as the one the engine hands out: downloads it
    when the version differs, swaps it in and lets the receiver restart. True when this PC has
    a receiver it can run (old or new)."""
    have = find_receiver_exe() is not None
    if not package or not package.get("available"):
        return have
    if package.get("version") == cfg("receiver_version") and have:
        return True
    if time.time() - receiver_state["update_failed"] < 60:
        return have  # after a failed attempt, wait a minute
    receiver_state["update_failed"] = time.time()  # cleared below when it works
    RECEIVER_DIR.mkdir(parents=True, exist_ok=True)
    downloaded = []
    try:
        for f in package.get("files") or []:
            tmp = RECEIVER_DIR / (f["name"] + ".download")
            with urllib.request.urlopen(mosaic_url("/api/receiver/files/" + f["name"]), timeout=120) as r, open(tmp, "wb") as out:
                shutil.copyfileobj(r, out, 1 << 20)
            downloaded.append(tmp)
            if tmp.stat().st_size != int(f["bytes"]):
                raise OSError("incomplete download of %s" % f["name"])
    except (urllib.error.URLError, OSError, ValueError) as exc:
        endpoint_state["receiver_update"] = "download failed: %s" % getattr(exc, "reason", exc)
        log.error("Mosaic Receiver update failed: %s", endpoint_state["receiver_update"])
        for tmp in downloaded:
            tmp.unlink(missing_ok=True)
        return have
    # The running receiver keeps its files locked (Windows): stop it, swap, and it's started again.
    was_running = receiver_call("GET", "/health") is not None
    if was_running:
        stop_receiver()
        time.sleep(1)
    try:
        for tmp in downloaded:
            os.replace(tmp, tmp.with_suffix(""))
    except OSError as exc:
        endpoint_state["receiver_update"] = "couldn't replace the receiver files: %s" % exc
        log.error("Mosaic Receiver update: %s", endpoint_state["receiver_update"])
        return find_receiver_exe() is not None
    exe = find_receiver_exe()
    if exe and not IS_WINDOWS:
        os.chmod(exe, 0o755)
    with config_lock:
        config["receiver_version"] = package["version"]
    save_config()
    endpoint_state["receiver_update"] = ""
    receiver_state["update_failed"] = 0.0
    log.info("Mosaic Receiver %s from the mosaic engine (version %s)", "updated" if have else "installed", package["version"][:8])
    return True


def find_receiver_exe():
    name = "mosaic_receiver.exe" if IS_WINDOWS else "mosaic_receiver"
    for rel in ("build/Release", "build", "build/RelWithDebInfo"):
        candidate = MOSAIC_DIR / rel / name
        if candidate.is_file():
            return str(candidate)
    return None


def receiver_call(method, path, body=None, timeout=1.5):
    try:
        return http_json(method, "http://127.0.0.1:%d%s" % (RECEIVER_PORT, path), body, timeout=timeout)
    except (urllib.error.URLError, OSError, ValueError):
        return None


def start_receiver():
    """Starts the receiver unless it answers already. True when it's up."""
    if receiver_call("GET", "/health"):
        return True
    exe = find_receiver_exe()
    if not exe:
        if time.time() - receiver_state["warned"] > 300:
            receiver_state["warned"] = time.time()
            log.warning("Mosaic Receiver not built on this PC (looked in %s); run update.bat", MOSAIC_DIR / "build")
        return False
    if time.time() - receiver_state["last_start"] < 10:
        return False  # just started one; give it time
    stop_receiver()
    LOG_DIR.mkdir(exist_ok=True)
    out = open(LOG_DIR / "mosaic-receiver.log", "ab")
    args = [exe, "--port", str(RECEIVER_PORT)]
    log.info("Starting Mosaic Receiver: %s", " ".join(args))
    receiver_state["last_start"] = time.time()
    receiver_state["sent"] = None  # a new process needs its displays again
    try:
        kwargs = dict(cwd=str(Path(exe).parent), stdin=subprocess.DEVNULL, stdout=out, stderr=out, close_fds=True)
        if IS_WINDOWS:
            receiver_state["proc"] = subprocess.Popen(args, creationflags=CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, **kwargs)
        else:
            receiver_state["proc"] = subprocess.Popen(args, **kwargs)
    except OSError as exc:
        log.error("Could not start the Mosaic Receiver: %s", exc)
        return False
    finally:
        out.close()
    for _ in range(20):
        time.sleep(0.25)
        if receiver_call("GET", "/health"):
            return True
    log.error("Mosaic Receiver did not answer (see logs/mosaic-receiver.log)")
    return False


def stop_receiver():
    proc = receiver_state["proc"]
    receiver_state["proc"] = None
    receiver_state["sent"] = None
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
    elif IS_WINDOWS:
        subprocess.run(["taskkill", "/F", "/IM", "mosaic_receiver.exe"], capture_output=True, timeout=15,
                       creationflags=CREATE_NO_WINDOW)


def receiver_displays(displays):
    """The receiver's config: each display slot on a screen (pinned, or the next free one;
    the main screen last when there are more screens than displays)."""
    screens = enumerate_displays()
    pinned = {d.get("screen") for d in displays if d.get("screen")}
    free = [s for s in screens if s["device"] not in pinned]
    if len(screens) > len(displays):
        free.sort(key=lambda s: s.get("primary", False))
    out = []
    for i, d in enumerate(displays):
        screen = next((s for s in screens if s["device"] == d.get("screen")), None) or (free.pop(0) if free else None)
        # No screen info (not Windows): side by side, for testing.
        rect = ({"x": screen["x"], "y": screen["y"], "w": screen["width"], "h": screen["height"]} if screen
                else {"x": i * 1920, "y": 0, "w": 1920, "h": 1080})
        slot = {"index": d.get("index", i + 1), "stream": d["stream"], "label": d.get("label") or "",
                "window": rect, "screen": screen["device"] if screen else ""}
        if d.get("crop"):  # this screen shows part of the stream (a 2 x 2 from one stream)
            slot["crop"] = d["crop"]
        out.append(slot)
    return out


def identify_displays():
    """Identify: every screen of this PC shows its number (as in the editor's Screen lists), its
    size and which display it's set up as, instead of a stream."""
    slots = cfg("receiver_displays") or []
    screens = enumerate_displays()
    if not screens:  # no screen info (not Windows): the displays' own windows, for testing
        screens = [{"device": d.get("screen") or "", "short": d.get("screen") or "DISPLAY%d" % (i + 1), "x": d["window"]["x"],
                    "y": d["window"]["y"], "width": d["window"]["w"], "height": d["window"]["h"]} for i, d in enumerate(slots)]
    host = (endpoint_state["endpoint"] or {}).get("name") or short_host(socket.gethostname())
    out = []
    for i, s in enumerate(screens):
        number = re.search(r"(\d+)$", s["short"])
        slot = next((d for d in slots if d.get("screen") == s["device"] and
                     (s["device"] or d["window"] == {"x": s["x"], "y": s["y"], "w": s["width"], "h": s["height"]})), None)
        lines = [host, "SCREEN %s  %dx%d%s" % (s["short"], s["width"], s["height"], "  (main)" if s.get("primary") else ""),
                 "DISPLAY %d: %s" % (slot["index"], slot.get("label") or slot["stream"]) if slot else "NOT USED"]
        if s.get("monitor"):
            lines.append(s["monitor"])
        out.append({"index": slot["index"] if slot else 100 + i, "stream": "identify", "label": slot.get("label", "") if slot else "",
                    "screen": s["device"], "window": {"x": s["x"], "y": s["y"], "w": s["width"], "h": s["height"]},
                    "identify": {"big": number.group(1) if number else str(i + 1), "lines": lines}})
    return out


def sync_receiver(displays=None):
    """Keeps the receiver running with the right displays (saved, so it comes back after a reboot)."""
    if displays is not None:
        with config_lock:
            config["receiver_displays"] = receiver_displays([d for d in displays if d.get("stream")])
        save_config()
    if not start_receiver():
        return
    endpoint = endpoint_state["endpoint"] or {}
    identify = bool(endpoint.get("identify"))
    wanted = {"displays": identify_displays() if identify else cfg("receiver_displays") or [],
              "flashTest": bool(endpoint.get("flashTest")) and not identify, "bufferMs": int(endpoint.get("bufferMs") or 0)}
    if receiver_state["sent"] != wanted:
        if receiver_call("PUT", "/config", wanted, timeout=3) is not None:
            receiver_state["sent"] = wanted
            if identify:
                log.info("Mosaic Receiver: identify, %d screen(s) show their numbers", len(wanted["displays"]))
            else:
                log.info("Mosaic Receiver: %s", ", ".join("%s -> %s" % (d["label"] or d["index"], d["stream"]) for d in wanted["displays"]) or "no displays")


def receiver_panels():
    """What each display shows, from the receiver, in the dashboard's panel shape."""
    status = receiver_call("GET", "/status") or {}
    shown = {d.get("index"): d for d in status.get("displays") or []}
    out = []
    for i, d in enumerate(cfg("receiver_displays") or [], start=1):
        st = shown.get(d.get("index")) or {}
        ok = st.get("status") == "ok"
        out.append({"index": i, "port": RECEIVER_PORT, "name": d.get("label") or "Display %d" % i, "screen": d.get("screen", ""),
                    "ndi_source": d["stream"], "playing": d["stream"] if ok else "", "reachable": bool(status),
                    "status": st.get("status", "receiver not running"), "frame": st.get("shownFrameId", 0)})
    return out, status


def set_player(player):
    """Switches this PC between Studio Monitor and the Mosaic Receiver."""
    if (cfg("player") or "studio_monitor") == player:
        return
    with config_lock:
        config["player"] = player
    save_config()
    log.info("Player is now %s", "Mosaic Receiver" if player == "mosaic" else "NDI Studio Monitor")
    if player == "mosaic":
        if processes_managed():
            with action_lock:
                kill_all_instances()
    else:
        stop_receiver()
        with config_lock:
            config["receiver_displays"] = []
        save_config()
        if processes_managed():
            restart_all("player switched back to Studio Monitor")


# --------------------------------------------------------------------------
# Self-update: "Update everything" in the mosaic engine's editor
# --------------------------------------------------------------------------

update_state = {"busy": False}


def app_version():
    """The checked-out commit, short, read from .git (no git process)."""
    git = BASE_DIR / ".git"
    try:
        head = (git / "HEAD").read_text().strip()
        if head.startswith("ref: "):
            ref = head[5:]
            if (git / ref).exists():
                head = (git / ref).read_text().strip()
            else:
                for line in (git / "packed-refs").read_text().splitlines():
                    if line.endswith(" " + ref):
                        head = line.split()[0]
        return head[:7]
    except OSError:
        return ""


def handle_update_request(request):
    """The engine asks every receiving PC to update (request id). Each id is acted on once."""
    if not request or update_state["busy"]:
        return
    rid = int(request.get("id") or 0)
    if rid <= int(cfg("update_id") or 0):
        return
    with config_lock:
        config["update_id"] = rid
    save_config()
    update_state["busy"] = True
    run_in_background(self_update)


def self_update():
    """git pull; when that brought a new version, restart this dashboard on it. The Mosaic
    Receiver isn't touched here: it comes from the engine once the media server has rebuilt."""
    before = app_version()
    set_update_result("updating", "getting the latest version")
    try:
        r = subprocess.run(["git", "-C", str(BASE_DIR), "pull", "--ff-only", "--autostash"], capture_output=True, text=True,
                           timeout=180, creationflags=CREATE_NO_WINDOW if IS_WINDOWS else 0)
    except (OSError, subprocess.TimeoutExpired) as exc:
        set_update_result("failed", "git pull didn't run (%s)" % exc)
        return
    if r.returncode != 0:
        lines = (r.stderr or r.stdout or "").strip().splitlines()
        set_update_result("failed", "git pull failed: %s" % (lines[-1] if lines else "exit code %d" % r.returncode))
        return
    after = app_version()
    if after == before:
        set_update_result("updated", "already up to date (%s)" % after)
        return
    set_update_result("updated", "updated %s -> %s" % (before, after))
    log.info("Updated %s -> %s; restarting the dashboard", before, after)
    restart_self()


def set_update_result(state_name, message):
    update_state["busy"] = state_name == "updating"
    with config_lock:
        config["update_result"] = {"id": int(config.get("update_id") or 0), "state": state_name, "message": message,
                                   "time": time.strftime("%Y-%m-%d %H:%M:%S")}
    save_config()
    if state_name == "failed":
        log.error("Update: %s", message)


def restart_self():
    """Start a new copy of this dashboard (outside this process's scheduled-task job, so it stays
    when this one exits), then exit. The new copy waits for the port to come free."""
    args = [sys.executable] + sys.argv
    kwargs = {"cwd": str(BASE_DIR), "close_fds": True}
    if IS_WINDOWS:
        flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
        try:
            subprocess.Popen(args, creationflags=flags | CREATE_BREAKAWAY_FROM_JOB, **kwargs)
        except OSError:
            subprocess.Popen(args, creationflags=flags, **kwargs)
    else:
        subprocess.Popen(args, start_new_session=True, **kwargs)
    time.sleep(1)
    os._exit(0)


def endpoint_checkin():
    screens = enumerate_displays()
    note_screens(screens)
    body = {"host": socket.gethostname(), "version": "videowall/1", "dashboardPort": int(cfg("dashboard_port")),
            "screens": screens, "player": cfg("player") or "studio_monitor", "diagnostics": diagnostics_summary(),
            "receiverVersion": cfg("receiver_version") or "", "receiverUpdate": endpoint_state.get("receiver_update") or "",
            "appVersion": app_version(), "update": cfg("update_result") or {}}
    if receiver_mode():
        panels_now, rx = receiver_panels()
        body["panels"] = panels_now
        body["receiver"] = rx or {"error": "the Mosaic Receiver isn't running"}
    else:
        body["panels"] = endpoint_report()
    try:
        reply = http_json("POST", mosaic_url("/api/endpoints/checkin"), body, timeout=3)
    except (urllib.error.URLError, OSError, ValueError) as exc:
        # Engine unreachable: keep showing what we have.
        endpoint_state["error"] = "mosaic engine not reachable (%s)" % getattr(exc, "reason", exc)
        return
    endpoint_state["error"] = None
    endpoint_state["last_ok"] = time.strftime("%H:%M:%S")
    handle_update_request(reply.get("update"))
    was = endpoint_state["endpoint"]
    endpoint_state["endpoint"] = reply.get("endpoint")
    endpoint_state["displays"] = reply.get("displays") or []
    if reply.get("endpoint") is None:
        if was:
            log.info("No longer an endpoint in the mosaic engine; panels stay as they are")
        return
    if not was:
        log.info("Managed by the mosaic engine as endpoint '%s'", reply["endpoint"].get("name"))
    # Keep the receiver current (it's downloaded from the engine), and only switch to it when this
    # PC has one: otherwise Studio Monitor stays, so the screens never go black.
    ready = update_receiver(reply.get("receiverPackage"))
    player = reply["endpoint"].get("player") or "studio_monitor"
    if player == "mosaic" and not ready:
        endpoint_state["receiver_update"] = endpoint_state.get("receiver_update") or "the mosaic engine has no receiver to hand out (build it on the media server)"
        player = "studio_monitor"
    set_player(player)
    if receiver_mode():
        sync_receiver(endpoint_state["displays"])
    else:
        sync_endpoint(endpoint_state["displays"])


def endpoint_loop():
    while state["phase"] != "running":  # after the startup sequence
        time.sleep(1)
    while True:
        try:
            endpoint_checkin()
        except Exception:
            log.exception("Endpoint check-in failed")
        time.sleep(ENDPOINT_INTERVAL)


# --------------------------------------------------------------------------
# Supervisor / watchdog
# --------------------------------------------------------------------------

action_lock = threading.Lock()
state = {
    "phase": "starting",
    "process_count": None,
    "last_check": None,
    "last_restart": 0.0,
    "unreachable_streak": {},
}


def processes_managed():
    return bool(cfg("manage_processes")) and IS_WINDOWS


def restart_all(reason):
    with action_lock:
        log.warning("Restarting all Studio Monitor instances: %s", reason)
        state["phase"] = "restarting"
        state["last_restart"] = time.time()
        kill_all_instances()
        time.sleep(2)
        ports = [p["port"] for p in panels()]
        if launch_instances(len(ports)):
            wait_for_api(ports, cfg("api_ready_timeout_seconds"))
            apply_all()
        state["unreachable_streak"] = {}
        state["phase"] = "running"


def initial_startup():
    delay = float(cfg("boot_delay_seconds"))
    state["phase"] = "boot-delay"
    log.info("Waiting %.0fs for network and displays to initialize", delay)
    time.sleep(delay)
    if mosaic_cfg().get("enabled"):
        # The engine first, so its streams exist when the monitors look for them.
        state["phase"] = "starting engine"
        start_mosaic()
    if receiver_mode():
        # Our own player: it shows every display; no Studio Monitor windows.
        state["phase"] = "starting receiver"
        sync_receiver()
        state["phase"] = "running"
        return
    with action_lock:
        state["phase"] = "launching"
        ports = [p["port"] for p in panels()]
        if processes_managed():
            running = count_processes() or 0
            state["process_count"] = running
            log.info("Found %d running Studio Monitor instance(s), need %d", running, len(ports))
            if running < len(ports):
                launch_instances(len(ports) - running)
        else:
            log.info("Process management disabled; only applying configuration")
        wait_for_api(ports, cfg("api_ready_timeout_seconds"))
        log.info("Applying saved mapping")
        apply_all()
        state["phase"] = "running"


def watchdog_tick():
    try:
        mosaic_watchdog()
    except Exception:
        log.exception("Mosaic engine check failed")
    if receiver_mode():
        sync_receiver()  # restarts it if it stopped, and gives it its displays again
        return
    expected = panels()
    if processes_managed():
        running = count_processes()
        state["process_count"] = running
        if running is not None and running < len(expected):
            if not exe_or_warn():
                return
            with action_lock:
                missing = len(expected) - running
                log.warning("Only %d/%d Studio Monitor instances running; launching %d",
                            running, len(expected), missing)
                if launch_instances(missing):
                    wait_for_api([p["port"] for p in expected], cfg("api_ready_timeout_seconds"))
                    apply_all()
            return

    streaks = state["unreachable_streak"]
    for panel in expected:
        current = get_configuration(panel["port"])
        if current is None:
            streaks[panel["port"]] = streaks.get(panel["port"], 0) + 1
            continue
        streaks[panel["port"]] = 0
        if (cfg("enforce_mapping") and panel.get("ndi_source")
                and current.get("NDI_source") != panel["ndi_source"]):
            log.info("%s drifted to '%s'; re-applying '%s'",
                     panel["name"], current.get("NDI_source"), panel["ndi_source"])
            with action_lock:
                apply_panel(panel)

    limit = int(cfg("hung_restart_after_checks"))
    hung = [port for port, n in streaks.items() if n >= limit]
    cooled_down = time.time() - state["last_restart"] > float(cfg("restart_cooldown_seconds"))
    if hung and processes_managed() and cooled_down:
        restart_all("API unresponsive on port(s) %s" % sorted(hung))


def supervisor_loop():
    try:
        initial_startup()
    except Exception:
        log.exception("Startup sequence failed")
        state["phase"] = "running"
    while True:
        time.sleep(float(cfg("watchdog_interval_seconds")))
        try:
            watchdog_tick()
            state["last_check"] = time.strftime("%H:%M:%S")
        except Exception:
            log.exception("Watchdog tick failed")


def run_in_background(fn, *args):
    threading.Thread(target=fn, args=args, daemon=True).start()


# --------------------------------------------------------------------------
# Display enumeration (Windows)
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# Diagnostics: GPU load, Windows display-driver events, screen changes
# --------------------------------------------------------------------------
# For tracing freezes on the wall from the editor: what the GPU was doing each second, what the
# display driver reported to Windows, and when the screens changed. GET /api/diagnostics has it
# all (with the receiver's own log); the check-in carries a summary.

diag = {"gpu": deque(maxlen=600), "gpu_error": None, "events": [], "events_error": None,
        "screen_changes": deque(maxlen=50), "screens": None}

# NVML clock-throttle reason bits that mean the GPU was slowed down (idle and app clocks aren't).
THROTTLE_REASONS = {0x4: "power cap", 0x8: "hardware slowdown", 0x20: "thermal (driver)",
                    0x40: "thermal (hardware)", 0x80: "power brake", 0x100: "display clock"}


def open_nvml():
    """NVIDIA's management library (ships with the driver), or None."""
    import ctypes
    paths = [os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "nvml.dll"),
             os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "NVIDIA Corporation", "NVSMI", "nvml.dll")]
    for path in paths:
        if os.path.exists(path):
            nvml = ctypes.CDLL(path)
            if nvml.nvmlInit_v2() == 0:
                return nvml
    return None


def gpu_loop():
    """Samples the (first) NVIDIA GPU once a second: load, memory, temperature, clocks, throttling."""
    import ctypes
    try:
        nvml = open_nvml()
    except OSError as exc:
        nvml, diag["gpu_error"] = None, "cannot load nvml.dll (%s)" % exc
    if nvml is None:
        diag["gpu_error"] = diag["gpu_error"] or "no NVIDIA GPU (nvml.dll) found"
        return
    handle = ctypes.c_void_p()
    if nvml.nvmlDeviceGetHandleByIndex_v2(0, ctypes.byref(handle)) != 0:
        diag["gpu_error"] = "NVML found no GPU"
        return
    while True:
        try:
            sample_gpu(nvml, handle)
        except Exception as exc:  # a driver update can pull NVML away: report it, keep trying
            diag["gpu_error"] = "GPU sampling failed (%s)" % exc
        time.sleep(1)


def sample_gpu(nvml, handle):
    import ctypes

    class Utilization(ctypes.Structure):
        _fields_ = [("gpu", ctypes.c_uint), ("memory", ctypes.c_uint)]

    class Memory(ctypes.Structure):
        _fields_ = [("total", ctypes.c_ulonglong), ("free", ctypes.c_ulonglong), ("used", ctypes.c_ulonglong)]

    util, mem = Utilization(), Memory()
    temp, clock, pstate, reasons = ctypes.c_uint(), ctypes.c_uint(), ctypes.c_int(), ctypes.c_ulonglong()
    if nvml.nvmlDeviceGetUtilizationRates(handle, ctypes.byref(util)) != 0:
        return
    nvml.nvmlDeviceGetMemoryInfo(handle, ctypes.byref(mem))
    nvml.nvmlDeviceGetTemperature(handle, 0, ctypes.byref(temp))
    nvml.nvmlDeviceGetClockInfo(handle, 0, ctypes.byref(clock))  # graphics clock
    nvml.nvmlDeviceGetPerformanceState(handle, ctypes.byref(pstate))
    nvml.nvmlDeviceGetCurrentClocksThrottleReasons(handle, ctypes.byref(reasons))
    diag["gpu"].append({"t": time.time(), "util": util.gpu, "memUtil": util.memory,
                        "memUsedMB": mem.used // 2**20, "memTotalMB": mem.total // 2**20,
                        "tempC": temp.value, "clockMHz": clock.value, "pstate": pstate.value,
                        "throttle": [n for bit, n in THROTTLE_REASONS.items() if reasons.value & bit]})


def gpu_summary():
    samples = [g for g in list(diag["gpu"]) if g["t"] > time.time() - 60]
    if not samples:
        return {"error": diag["gpu_error"]} if diag["gpu_error"] else None
    now = samples[-1]
    return {"util": now["util"], "utilMax60": max(g["util"] for g in samples),
            "utilAvg60": round(sum(g["util"] for g in samples) / len(samples)),
            "memUsedMB": now["memUsedMB"], "memTotalMB": now["memTotalMB"], "tempC": now["tempC"],
            "tempMax60": max(g["tempC"] for g in samples), "clockMHz": now["clockMHz"], "pstate": now["pstate"],
            "throttle60": sorted({r for g in samples for r in g["throttle"]})}


# Windows logs that tell about the display stack: the NVIDIA driver (resets, errors), the
# graphics kernel (TDR: a GPU hang and recovery), "Display" (driver stopped responding), and
# programs crashing (dwm.exe among them).
DRIVER_EVENT_QUERIES = [
    ("System", "nvlddmkm", "Display", "Microsoft-Windows-DxgKrnl"),
    ("Application", "Application Error", "Application Hang"),
]


def read_driver_events(hours=24):
    import xml.etree.ElementTree as ET
    from datetime import datetime
    ns = {"e": "http://schemas.microsoft.com/win/2004/08/events/event"}
    events = []
    for log_name, *providers in DRIVER_EVENT_QUERIES:
        who = " or ".join("@Name='%s'" % p for p in providers)
        query = "*[System[Provider[%s] and TimeCreated[timediff(@SystemTime) <= %d]]]" % (who, hours * 3600 * 1000)
        r = subprocess.run(["wevtutil", "qe", log_name, "/q:" + query, "/f:RenderedXml", "/rd:true", "/c:40"],
                           capture_output=True, text=True, timeout=20, creationflags=CREATE_NO_WINDOW)
        if r.returncode != 0:
            raise RuntimeError((r.stderr or r.stdout or "wevtutil failed").strip())
        for ev in ET.fromstring("<Events>%s</Events>" % r.stdout).findall("e:Event", ns):
            system = ev.find("e:System", ns)
            when = system.find("e:TimeCreated", ns).get("SystemTime", "")
            # 2026-11-05T23:08:42.1234567Z: Python wants at most 6 fraction digits
            when = re.sub(r"(\.\d{6})\d*", r"\1", when).replace("Z", "+00:00")
            try:
                local = datetime.fromisoformat(when).astimezone()
            except ValueError:
                continue
            message = ev.findtext("e:RenderingInfo/e:Message", "", ns) or ""
            events.append({"t": local.timestamp(), "at": local.strftime("%Y-%m-%d %H:%M:%S"), "log": log_name,
                           "source": system.find("e:Provider", ns).get("Name", ""),
                           "id": int(system.findtext("e:EventID", "0", ns) or 0),
                           "level": {"1": "critical", "2": "error", "3": "warning"}.get(system.findtext("e:Level", "", ns), "info"),
                           "message": " ".join(message.split())[:300]})
    return sorted(events, key=lambda e: e["t"], reverse=True)


def driver_events_loop():
    while True:
        try:
            diag["events"] = read_driver_events()
            diag["events_error"] = None
        except Exception as exc:  # wevtutil missing, access denied, odd XML: report, keep trying
            diag["events_error"] = str(exc)[:200]
        time.sleep(60)


def note_screens(screens):
    """Records when the screens change (a monitor dropping out or coming back, a new resolution)."""
    shape = sorted((d.get("device"), d.get("x"), d.get("y"), d.get("width"), d.get("height")) for d in screens)
    if diag["screens"] is not None and shape != diag["screens"]:
        before = {s[0] for s in diag["screens"]}
        after = {s[0] for s in shape}
        what = ", ".join(["%s gone" % d for d in sorted(before - after)] + ["%s back" % d for d in sorted(after - before)]) or "positions or resolutions changed"
        diag["screen_changes"].append({"t": time.time(), "at": time.strftime("%Y-%m-%d %H:%M:%S"), "change": what})
        log.warning("Screens changed: %s", what)
    diag["screens"] = shape


def receiver_log_tail(lines=300):
    path = LOG_DIR / "mosaic-receiver.log"
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - 256 * 1024))
            text = f.read().decode("utf-8", "replace")
    except OSError:
        return []
    return text.splitlines()[-lines:]


def diagnostics_summary():
    hour = time.time() - 3600
    return {"gpu": gpu_summary(),
            "driverEvents": [e for e in diag["events"] if e["t"] > hour][:10],
            "driverEventsError": diag["events_error"],
            "screenChanges": [c for c in diag["screen_changes"] if c["t"] > hour][-10:]}


def diagnostics_payload():
    return dict(diagnostics_summary(), gpuSamples=list(diag["gpu"]), driverEvents24h=diag["events"],
                screenChanges=list(diag["screen_changes"]), receiverLog=receiver_log_tail())


def enumerate_displays():
    if not IS_WINDOWS:
        return []
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    try:
        user32.SetProcessDPIAware()
    except AttributeError:
        pass

    # Each screen's real position and size, in pixels, from its display mode. GetMonitorInfo's
    # numbers are scaled by Windows whenever the screens don't all use the same Scale (100 %,
    # 150 %, ...), and a window placed by them lands on the wrong part of the wall.
    class DEVMODEW(ctypes.Structure):
        _fields_ = [("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD),
                    ("dmDriverVersion", wintypes.WORD), ("dmSize", wintypes.WORD),
                    ("dmDriverExtra", wintypes.WORD), ("dmFields", wintypes.DWORD),
                    ("dmPositionX", wintypes.LONG), ("dmPositionY", wintypes.LONG),
                    ("dmDisplayOrientation", wintypes.DWORD), ("dmDisplayFixedOutput", wintypes.DWORD),
                    ("dmColor", ctypes.c_short), ("dmDuplex", ctypes.c_short),
                    ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short),
                    ("dmCollate", ctypes.c_short), ("dmFormName", wintypes.WCHAR * 32),
                    ("dmLogPixels", wintypes.WORD), ("dmBitsPerPel", wintypes.DWORD),
                    ("dmPelsWidth", wintypes.DWORD), ("dmPelsHeight", wintypes.DWORD),
                    ("dmDisplayFlags", wintypes.DWORD), ("dmDisplayFrequency", wintypes.DWORD),
                    ("dmICMMethod", wintypes.DWORD), ("dmICMIntent", wintypes.DWORD),
                    ("dmMediaType", wintypes.DWORD), ("dmDitherType", wintypes.DWORD),
                    ("dmReserved1", wintypes.DWORD), ("dmReserved2", wintypes.DWORD),
                    ("dmPanningWidth", wintypes.DWORD), ("dmPanningHeight", wintypes.DWORD)]

    def real_rect(device):
        mode = DEVMODEW()
        mode.dmSize = ctypes.sizeof(DEVMODEW)
        try:
            user32.EnumDisplaySettingsW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, ctypes.POINTER(DEVMODEW)]
            if user32.EnumDisplaySettingsW(device, 0xFFFFFFFF, ctypes.byref(mode)) and mode.dmPelsWidth and mode.dmPelsHeight:  # ENUM_CURRENT_SETTINGS
                return mode.dmPositionX, mode.dmPositionY, int(mode.dmPelsWidth), int(mode.dmPelsHeight)
        except Exception:
            pass
        return None

    class DISPLAY_DEVICEW(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("DeviceName", wintypes.WCHAR * 32),
                    ("DeviceString", wintypes.WCHAR * 128), ("StateFlags", wintypes.DWORD),
                    ("DeviceID", wintypes.WCHAR * 128), ("DeviceKey", wintypes.WCHAR * 128)]

    class MONITORINFOEXW(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT),
                    ("rcWork", wintypes.RECT), ("dwFlags", wintypes.DWORD),
                    ("szDevice", wintypes.WCHAR * 32)]

    MonitorEnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HMONITOR, wintypes.HDC,
                                         ctypes.POINTER(wintypes.RECT), wintypes.LPARAM)
    user32.EnumDisplayDevicesW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                           ctypes.POINTER(DISPLAY_DEVICEW), wintypes.DWORD]
    user32.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p, MonitorEnumProc,
                                           wintypes.LPARAM]
    user32.GetMonitorInfoW.argtypes = [wintypes.HMONITOR, ctypes.POINTER(MONITORINFOEXW)]

    friendly = {}
    i = 0
    while True:
        adapter = DISPLAY_DEVICEW()
        adapter.cb = ctypes.sizeof(adapter)
        if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(adapter), 0):
            break
        if adapter.StateFlags & 0x1:  # DISPLAY_DEVICE_ATTACHED_TO_DESKTOP
            monitor = DISPLAY_DEVICEW()
            monitor.cb = ctypes.sizeof(monitor)
            name = ""
            if user32.EnumDisplayDevicesW(adapter.DeviceName, 0, ctypes.byref(monitor), 0):
                name = monitor.DeviceString
            friendly[adapter.DeviceName] = (name or adapter.DeviceString, adapter.DeviceString)
        i += 1

    displays = []

    def callback(hmonitor, hdc, rect, lparam):
        info = MONITORINFOEXW()
        info.cbSize = ctypes.sizeof(info)
        if user32.GetMonitorInfoW(hmonitor, ctypes.byref(info)):
            r = info.rcMonitor
            x, y, w, h = real_rect(info.szDevice) or (r.left, r.top, r.right - r.left, r.bottom - r.top)
            monitor_name, adapter_name = friendly.get(info.szDevice, ("", ""))
            displays.append({
                "device": info.szDevice,
                "short": info.szDevice.replace("\\\\.\\", ""),
                "monitor": monitor_name,
                "adapter": adapter_name,
                "x": x, "y": y,
                "width": w, "height": h,
                "primary": bool(info.dwFlags & 0x1),
            })
        return True

    user32.EnumDisplayMonitors(None, None, MonitorEnumProc(callback), 0)
    displays.sort(key=lambda d: (d["y"], d["x"]))
    return displays


# --------------------------------------------------------------------------
# Dashboard HTTP server
# --------------------------------------------------------------------------

def status_payload():
    result = []
    if receiver_mode():
        for p in receiver_panels()[0]:
            result.append(dict(p, reachable=p["status"] == "ok", current_source=p["playing"] or None,
                               current_display=p["screen"], current_config=None))
    for p in ([] if receiver_mode() else panels()):
        current = get_configuration(p["port"])
        window = (current or {}).get("window") or {}
        result.append(dict(p, reachable=current is not None,
                           current_source=(current or {}).get("NDI_source"),
                           current_display=window.get("display_device"),
                           current_config=current))
    return {
        "panels": result,
        "phase": state["phase"],
        "process_count": state["process_count"] if processes_managed() and not receiver_mode() else None,
        "processes_managed": processes_managed(),
        "exe_path": find_exe() if processes_managed() else None,
        "exe_configured": cfg("exe_path"),
        "exe_search_roots": search_roots(),
        "last_check": state["last_check"],
        "enforce_mapping": cfg("enforce_mapping"),
        "layout_columns": cfg("layout_columns"),
        "fullscreen": cfg("fullscreen"),
        "screens": enumerate_displays(),
        "hostname": socket.gethostname(),
        "player": cfg("player") or "studio_monitor",
        "endpoint": {"managed": endpoint_managed(), "name": (endpoint_state["endpoint"] or {}).get("name"),
                     "error": endpoint_state["error"], "last_ok": endpoint_state["last_ok"]},
    }


def update_panels(updates):
    with config_lock:
        by_port = {p["port"]: p for p in config["panels"]}
        for u in updates:
            panel = by_port.get(int(u["port"]))
            if not panel:
                raise ValueError("Unknown panel port %s" % u["port"])
            for key in ("ndi_source", "display_device", "name"):
                if key in u and u[key] is not None:
                    panel[key] = str(u[key]).strip()
    save_config()


class Handler(BaseHTTPRequestHandler):
    server_version = "VideoWallControl/1.0"

    def log_message(self, fmt, *args):
        log.debug("%s %s", self.address_string(), fmt % args)

    def send_json(self, obj, code=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        if not length:
            return {}
        return json.loads(self.rfile.read(length).decode("utf-8"))

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        try:
            if path in ("/", "/index.html"):
                body = (STATIC_DIR / "index.html").read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif path == "/api/status":
                self.send_json(status_payload())
            elif path == "/api/sources":
                self.send_json({"sources": get_sources()})
            elif path == "/api/sources/raw":
                port, raw = get_raw_sources()
                self.send_json({"port": port, "raw": raw})
            elif path == "/api/displays":
                self.send_json({"displays": enumerate_displays()})
            elif path == "/api/diagnostics":
                self.send_json(diagnostics_payload())
            elif path == "/api/log":
                self.send_json({"log": list(recent_log)})
            elif path == "/api/mosaic":
                host = (self.headers.get("Host") or "localhost").rsplit(":", 1)[0]
                self.send_json(mosaic_status(host))
            else:
                self.send_json({"error": "not found"}, 404)
        except RuntimeError as exc:
            self.send_json({"error": str(exc)}, 503)
        except Exception as exc:
            log.exception("GET %s failed", path)
            self.send_json({"error": str(exc)}, 500)

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        try:
            body = self.read_body()
            if path in ("/api/apply", "/api/mosaic/assign") and endpoint_managed():
                self.send_json({"error": "This PC is managed by the mosaic engine as endpoint '%s'; "
                                         "change its displays in the mosaic editor (Endpoints)"
                                         % endpoint_state["endpoint"].get("name")}, 409)
                return
            if path == "/api/apply":
                # body: {"panels": [{"port": 80, "ndi_source": "...", "display_device": "..."}]}
                updates = body.get("panels") or []
                update_panels(updates)
                ports = {int(u["port"]) for u in updates}
                with action_lock:
                    results = [apply_panel(p) for p in panels() if p["port"] in ports]
                self.send_json({"results": [{"ok": ok, "message": m} for ok, m in results]})
            elif path == "/api/reapply":
                with action_lock:
                    results = apply_all()
                self.send_json({"results": [{"ok": ok, "message": m} for ok, m in results]})
            elif path == "/api/snap":
                # Forget saved screens and re-detect from where each window is right now.
                with config_lock:
                    for p in config["panels"]:
                        p.pop("screen", None)
                save_config()
                log.info("Re-detecting screens from current window positions")
                with action_lock:
                    results = apply_all()
                self.send_json({"results": [{"ok": ok, "message": m} for ok, m in results]})
            elif path == "/api/restart":
                if not processes_managed():
                    self.send_json({"error": "Process management is disabled"}, 400)
                    return
                run_in_background(restart_all, "requested from dashboard")
                self.send_json({"ok": True})
            elif path == "/api/mosaic/restart":
                if mosaic_remote_host():
                    self.send_json({"error": "The engine runs on %s; restart it there" % mosaic_remote_host()}, 400)
                    return
                if not find_mosaic_exe():
                    self.send_json({"error": "Mosaic engine not found; build it or set its path"}, 400)
                    return
                def restart():
                    stop_mosaic()
                    start_mosaic()
                run_in_background(restart)
                self.send_json({"ok": True})
            elif path == "/api/mosaic/assign":
                results = assign_mosaic_to_panels()
                self.send_json({"results": [{"ok": ok, "message": m} for ok, m in results]})
            elif path == "/api/settings":
                with config_lock:
                    if "enforce_mapping" in body:
                        config["enforce_mapping"] = bool(body["enforce_mapping"])
                    if "exe_path" in body:
                        exe = str(body["exe_path"]).strip().strip('"')
                        if exe and not os.path.isfile(exe):
                            raise ValueError("File not found: %s" % exe)
                        config["exe_path"] = exe
                        exe_cache.update(path=None, checked=0.0, warned=0.0)
                        log.info("Studio Monitor path set to '%s'", exe or "(auto-detect)")
                    if "mosaic_enabled" in body or "mosaic_exe_path" in body or "mosaic_host" in body:
                        mc = dict(config.get("mosaic") or {})
                        if "mosaic_enabled" in body:
                            mc["enabled"] = bool(body["mosaic_enabled"])
                        if "mosaic_exe_path" in body:
                            exe = str(body["mosaic_exe_path"]).strip().strip('"')
                            if exe and not os.path.isfile(exe):
                                raise ValueError("File not found: %s" % exe)
                            mc["exe_path"] = exe
                        if "mosaic_host" in body:
                            mc["host"], port = parse_mosaic_host(body["mosaic_host"])
                            if port:
                                mc["port"] = port
                            log.info("Mosaic engine host set to '%s'", mc["host"] or "(this PC)")
                        config["mosaic"] = mc
                        log.info("Mosaic engine %s", "enabled" if mc.get("enabled", True) else "disabled")
                save_config()
                if "mosaic_enabled" in body or "mosaic_exe_path" in body or "mosaic_host" in body:
                    run_local = mosaic_cfg().get("enabled") and not mosaic_remote_host()
                    run_in_background(start_mosaic if run_local else stop_mosaic)
                self.send_json({"ok": True})
            else:
                self.send_json({"error": "not found"}, 404)
        except (ValueError, KeyError, TypeError) as exc:
            self.send_json({"error": str(exc)}, 400)
        except Exception as exc:
            log.exception("POST %s failed", path)
            self.send_json({"error": str(exc)}, 500)


def main():
    setup_logging(console="--console" in sys.argv)
    load_config()
    host, port = cfg("dashboard_host"), int(cfg("dashboard_port"))
    # After a self-update the previous copy may still hold the port for a moment.
    for attempt in range(20):
        try:
            server = ThreadingHTTPServer((host, port), Handler)
            break
        except OSError as exc:
            if attempt == 19:
                log.error("Cannot bind dashboard on %s:%d (%s). Already running?", host, port, exc)
                sys.exit(1)
            time.sleep(1)
    server.daemon_threads = True
    log.info("Video Wall Control started; dashboard on http://%s:%d", host, port)
    run_in_background(supervisor_loop)
    run_in_background(endpoint_loop)
    if IS_WINDOWS:
        run_in_background(gpu_loop)
        run_in_background(driver_events_loop)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        log.info("Shutting down")


if __name__ == "__main__":
    main()
