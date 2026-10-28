# Video Wall Control

A background service plus web dashboard for a 4-panel NDI mosaic driven by
4 instances of **NDI Studio Monitor** on a Windows Mini PC.

- Starts hidden at Windows logon (no console window on the wall)
- Waits 10 s for the network and displays, then launches any missing Studio Monitor
  instances 2 s apart so they bind to ports 80, 81, 82 and 83
- Applies the saved stream-to-panel mapping from `config.json` to each instance
- Watchdog: relaunches crashed instances, re-applies the mapping if a panel drifts
  to another source, and restarts all instances if an API stays unresponsive
- Dashboard at `http://<mini-pc-ip>:8080`: live state of each panel, NDI source
  discovery, display selection, apply/save, re-apply, restart, activity log

It uses only the Python standard library, so nothing needs to be installed with pip.

## Install on the Mini PC

1. Install **Python 3.8+** from python.org (tick *Add python.exe to PATH*).
   The Microsoft Store version won't work here; use the python.org installer.
2. Copy this folder somewhere permanent, e.g. `C:\VideoWallControl`.
3. Double-click **`install.bat`** and accept the admin prompt. It:
   - registers the Task Scheduler task `VideoWallControl`, which runs `pythonw.exe videowall.py`
     at logon with highest privileges and restarts it if it crashes
   - opens TCP 8080 in Windows Firewall
   - starts it immediately
4. **Turn on automatic sign-in** for the Windows account, for example with
   [Sysinternals Autologon](https://learn.microsoft.com/sysinternals/downloads/autologon).
   Studio Monitor needs a real desktop session to draw on, so this runs at
   logon rather than as a Session 0 Windows service.

To remove it, run `uninstall.bat`. That leaves the Studio Monitor windows running.

## Using the dashboard

Open `http://localhost:8080` on the Mini PC, or `http://<mini-pc-ip>:8080` from any
machine on the LAN.

- Each card is one panel (one Studio Monitor port). It shows what that panel is
  playing right now and whether it matches the saved mapping.
- Pick an **NDI source** and an **Output display** for each panel, then click
  **Apply to this panel** or **Save & apply all**. Applied choices are saved to `config.json`
  and restored on every boot.
- **Keep current display** leaves out `display_device`, so Studio Monitor keeps
  whatever display it's already on.
- **Enforce mapping** lets the watchdog put the saved source back if someone changes it
  in Studio Monitor's own UI. Turn it off if you want to make manual changes.
- **Restart monitors** kills all instances and relaunches them in order.

### Display names

The display list combines the Windows displays (`\\.\DISPLAY1`... with resolution and
desktop position, sorted top-to-bottom and left-to-right) with whatever value Studio
Monitor itself reports in `/v1/configuration` (labelled *reported by Studio Monitor*).
**Use the format Studio Monitor reports.** To find it, move each Studio Monitor window to
a display using its own settings once, then look at the *Display* line on each card.
You can also type any value with *Other (type manually)*.

## config.json

| Key | Default | Meaning |
|---|---|---|
| `dashboard_port` | `8080` | Dashboard port (80–83 belong to Studio Monitor) |
| `dashboard_host` | `0.0.0.0` | Use `127.0.0.1` to allow the dashboard only on the Mini PC itself |
| `exe_path` | `""` | Full path to `Application.Network.VideoMonitor2.x64.exe`. If empty, it's looked up in `exe_search_paths` (NDI 6 / NDI 5 / NewTek folders) |
| `boot_delay_seconds` | `10` | Wait before the first launch |
| `launch_gap_seconds` | `2` | Gap between instance launches |
| `watchdog_interval_seconds` | `15` | How often the watchdog checks |
| `enforce_mapping` | `true` | Re-apply the saved source when a panel drifts |
| `hung_restart_after_checks` | `4` | Restart all instances after this many failed API checks in a row (with a 5-minute cooldown) |
| `window_options` | `hide_border`, `always_on_top` = `true` | Sent as the `window` object |
| `decorations` | `hw_accel: true` | Sent as the `decorations` object |
| `layout_columns` | `2` | Dashboard grid. Use `4` for a 1×4 wall |
| `panels` | ports 80–83 | Panel name, port, saved `ndi_source` and `display_device` |

The POST sent to each `http://127.0.0.1:<port>/v1/configuration` looks like this:

```json
{
  "version": 1,
  "NDI_source": "RESOLUME-PC (Panel 1)",
  "window": { "display_device": "...", "hide_border": true, "always_on_top": true },
  "decorations": { "hw_accel": true }
}
```

Logs go to `logs\videowall.log` (rotated). The last 300 lines also appear in the dashboard.

## Running manually / testing without the wall

```bat
python videowall.py --console
```

To try the dashboard on any machine without NDI, use the bundled mock API:

```bash
python3 tools/mock_studio_monitor.py 9080 9081 9082 9083
```

Then point a test config at those ports with process management turned off:

```bash
VIDEOWALL_CONFIG=test-config.json python3 videowall.py --console
```

In that `test-config.json`, set `"manage_processes": false`, `"boot_delay_seconds": 1`, and panel ports 9080–9083.
