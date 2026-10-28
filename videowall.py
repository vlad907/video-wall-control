"""
Video Wall Control - background service + web dashboard for a 4-panel
NDI Studio Monitor mosaic.

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
            monitor_name, adapter_name = friendly.get(info.szDevice, ("", ""))
            displays.append({
                "device": info.szDevice,
                "short": info.szDevice.replace("\\\\.\\", ""),
                "monitor": monitor_name,
                "adapter": adapter_name,
                "x": r.left, "y": r.top,
                "width": r.right - r.left, "height": r.bottom - r.top,
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
    for p in panels():
        current = get_configuration(p["port"])
        window = (current or {}).get("window") or {}
        result.append(dict(p, reachable=current is not None,
                           current_source=(current or {}).get("NDI_source"),
                           current_display=window.get("display_device"),
                           current_config=current))
    return {
        "panels": result,
        "phase": state["phase"],
        "process_count": state["process_count"] if processes_managed() else None,
        "processes_managed": processes_managed(),
        "exe_path": find_exe() if processes_managed() else None,
        "exe_configured": cfg("exe_path"),
        "exe_search_roots": search_roots(),
        "last_check": state["last_check"],
        "enforce_mapping": cfg("enforce_mapping"),
        "layout_columns": cfg("layout_columns"),
        "fullscreen": cfg("fullscreen"),
        "screens": enumerate_displays(),
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
            elif path == "/api/log":
                self.send_json({"log": list(recent_log)})
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
                save_config()
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
    try:
        server = ThreadingHTTPServer((host, port), Handler)
    except OSError as exc:
        log.error("Cannot bind dashboard on %s:%d (%s). Already running?", host, port, exc)
        sys.exit(1)
    server.daemon_threads = True
    log.info("Video Wall Control started; dashboard on http://%s:%d", host, port)
    run_in_background(supervisor_loop)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        log.info("Shutting down")


if __name__ == "__main__":
    main()
