# Video Wall Control

A background service plus web dashboard for a 4-panel NDI mosaic driven by
4 instances of **NDI Studio Monitor** on a Windows Mini PC.

> This repo also contains [`mosaic-engine/`](mosaic-engine/), a GPU mosaic engine that
> cuts one NDI canvas into separate NDI outputs (`Mosaic_Output_01…`), to replace
> Resolume's Advanced Output. Once it's built, this service starts it at logon too, and the
> dashboard can point the panels at its outputs in one click. See its own README.

- Starts hidden at Windows logon (no console window on the wall)
- Waits 10 s for the network and displays, then launches any missing Studio Monitor
  instances 2 s apart so they bind to ports 80, 81, 82 and 83
- Applies the saved stream-to-panel mapping from `config.json` to each instance
- Watchdog: relaunches crashed instances, re-applies the mapping if a panel drifts
  to another source, and restarts all instances if an API stays unresponsive
- Dashboard at `http://<mini-pc-ip>:8080`: every NDI stream on the network, what each
  panel is playing, source selection, apply/save, re-apply, restart, activity log
- If `mosaic-engine/` has been built: starts the engine before the monitors, restarts it
  if it stops answering, and shows its status. **Show mosaic on panels** sets panel 1 to
  `Mosaic_Output_01`, panel 2 to `_02`, and so on. The engine's log is `logs/mosaic-engine.log`
- **Endpoint mode**: a receiving PC can be set up from the mosaic editor instead of here
  (see [Endpoint mode](#endpoint-mode-set-up-from-the-mosaic-editor))

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

## The media server (mosaic engine only)

On the PC that runs the mosaic engine and doesn't drive displays itself, build the engine
(`mosaic-engine/docs/WINDOWS.md`), then double-click **`install-engine.bat`**. It:

- registers the Task Scheduler task `MosaicEngine`, which starts the engine hidden at logon and
  restarts it if it exits or stops answering for 30 s (`mosaic-engine/run-engine.ps1`)
- opens TCP 8090 and allows the engine's NDI streams in Windows Firewall
- if this dashboard is installed on the same PC, turns off its *Run engine*, so the two don't both start one

Logs: `logs/mosaic-engine.log` (the engine) and `logs/mosaic-engine-watchdog.log` (starts and restarts).
`install-engine.bat -Uninstall` removes it. Turn on automatic sign-in here too.

## Updating

Double-click **`update.bat`** on any PC: media server or receiving PC. It runs `git pull`, rebuilds the
engine and the editor if they're built on that PC, and restarts whatever is installed (`VideoWallControl`,
`MosaicEngine`). The Studio Monitor windows stay up during the update. On the media server, the NDI
outputs pause for the few seconds the engine is being rebuilt. If a build fails, the previous engine is
started again, and the window shows what went wrong.

## Using the dashboard

Open `http://localhost:8080` on the Mini PC, or `http://<mini-pc-ip>:8080` from any
machine on the LAN.

- Each card is one panel (one Studio Monitor port). It shows what that panel is
  playing right now and whether it matches the saved mapping.
- The **NDI streams on the network** list shows every source found (refreshed every 10 s)
  and which panel, if any, is showing it. Streams are found two ways: through Studio
  Monitor's `/v1/sources`, and directly on the network over mDNS. The mDNS search works even
  when no Studio Monitor is running. If you use an NDI Discovery Server rather than mDNS,
  streams appear once at least one Studio Monitor is running.
- If the Studio Monitor program can't be found, a yellow banner appears with a field to
  paste its full path. In File Explorer, Shift + right-click the .exe and choose *Copy as path*.
- Pick an **NDI source** for each panel, then click **Apply to this panel** or
  **Save & apply all**. Applied choices are saved to `config.json` and restored on every boot.
  Until you save, each dropdown shows whatever the panel is playing right now.
- **Full screen:** each panel is made borderless and always on top, and its window is sized
  to exactly cover one screen. The first time, the screen is whichever one the window is
  mostly on; after that it's remembered in `config.json` (`screen`). To change which panel
  goes on which screen, drag each Studio Monitor window onto the screen you want and click
  **Snap to screens**.
- **Enforce mapping** lets the watchdog put the saved source back if someone changes it
  in Studio Monitor's own UI. Turn it off if you want to make manual changes.
- **Restart monitors** kills all instances and relaunches them in order.

### If no streams show up

Open `http://localhost:8080/api/sources/raw` on the Mini PC. It shows the raw
`/v1/sources` reply from Studio Monitor, which helps when the list comes back empty.

## Endpoint mode: set up from the mosaic editor

With several receiving PCs, set them all up in one place: the mosaic editor on the media server.

1. On each receiving PC, install this dashboard as usual and enter the media server's
   name or IP under **Engine PC** (Mosaic engine section), then click **Save**.
2. The PC now checks in with the engine every 5 seconds. In the editor
   (`http://<media-server>:8090`) it appears under **Endpoints** as "not set up", with its screens.
   Click **Set up**.
3. Pick the output for each display, or use **+ Whole group…** to add one display per panel of a
   wall group. A display can be pinned to one of the PC's screens, or take the next free one.
   When the PC has several screens, the main screen (usually the control screen) is left out at first.

Within a few seconds the PC starts one Studio Monitor per display, full screen on its screen, playing that output's
stream. Changes in the editor reach it on its next check-in. The editor shows each endpoint as online or offline, and
whether each display is playing the right stream.

While a PC is managed this way, its dashboard shows **Endpoint: <name> (set in the mosaic editor)**, and its source
pickers and *Save & apply all* are locked. Its panel list is saved in `config.json` as usual, so if the
media server is unreachable, the PC keeps showing what it showed last, and it starts the same way after a reboot.
Endpoints are matched by PC name: a replacement PC with the same name picks up the same setup.
Removing the endpoint in the editor hands the PC back to manual control.

**Player.** By default an endpoint shows its streams with NDI Studio Monitor windows. With
*Player: Mosaic Receiver* (endpoint panel in the editor), the PC uses our own player instead: one
process shows all its displays in step, and reports how far apart they are. It needs
`mosaic_receiver.exe` on that PC. See [mosaic-engine/docs/RECEIVER.md](mosaic-engine/docs/RECEIVER.md).

## config.json

| Key | Default | Meaning |
|---|---|---|
| `dashboard_port` | `8080` | Dashboard port (80–83 belong to Studio Monitor) |
| `dashboard_host` | `0.0.0.0` | Use `127.0.0.1` to allow the dashboard only on the Mini PC itself |
| `exe_path` | `""` | Full path to the Studio Monitor .exe. If empty, it's looked up in `exe_search_paths`, then anywhere under `Program Files\NDI`, `NewTek` or `Vizrt` (also in `Program Files (x86)`), then the target of any NDI "Studio Monitor" Start Menu shortcut |
| `mdns_discovery` | `true` | Also find NDI streams directly over mDNS |
| `fullscreen` | `true` | Size each window to exactly cover its screen (borderless, always on top) |
| `boot_delay_seconds` | `10` | Wait before the first launch |
| `launch_gap_seconds` | `2` | Gap between instance launches |
| `watchdog_interval_seconds` | `15` | How often the watchdog checks |
| `enforce_mapping` | `true` | Re-apply the saved source when a panel drifts |
| `hung_restart_after_checks` | `4` | Restart all instances after this many failed API checks in a row (with a 5-minute cooldown) |
| `window_options` | `hide_border`, `always_on_top` = `true` | Sent as the `window` object |
| `decorations` | `hw_accel: true` | Sent as the `decorations` object |
| `layout_columns` | `2` | Dashboard grid. Use `4` for a 1×4 wall |
| `panels` | ports 80–83 | Panel name, port, saved `ndi_source` (optional `display_device`, sent only if set) |
| `mosaic` | `enabled: true`, `port: 8090` | Mosaic engine supervision. `exe_path` empty means `mosaic-engine\build\Release\mosaic_engine.exe`; `args` are extra command-line flags (for example `["--layout", "wall-2x2-4k"]`). If the engine isn't built, this does nothing. `host` is the PC running the engine when it isn't this one (name or IP; the dashboard's **Engine PC** field): this PC then runs no engine of its own, and **Show mosaic on panels** reads that engine's outputs. Its port 8090 must be open in that PC's firewall (`install.bat` there adds the rule) |

Each apply reads the panel's current `/v1/configuration`, changes only these fields, and POSTs the whole object back:

```json
{
  "version": 1,
  "NDI_source": "RESOLUME-PC (Panel 1)",
  "window": { "hide_border": true, "always_on_top": true, "showcmd": 1,
              "normal_posn_left": 1920, "normal_posn_top": -2160,
              "normal_posn_right": 5760, "normal_posn_bottom": 0 },
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
