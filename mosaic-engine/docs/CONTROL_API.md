# Control API

How a control system (Bitfocus Companion, Crestron, Q-SYS, AMX, a custom app or a script)
switches what the video walls show, by sending HTTP requests to the mosaic engine on the
media server.

Everything here is plain HTTP with JSON. There is no login.

## Connection

| | |
|---|---|
| Base URL | `http://<media-server>:8090` (for example `http://192.168.1.187:8090`) |
| Method | `GET` to read, `POST` to change something |
| Body | JSON, or empty. Send `Content-Type: application/json` when there is a body |
| Replies | JSON. `200` / `201` on success; `400`, `401`, `404` or `500` with `{"error": "..."}` |
| Key | Only if an API key is set (editor → Settings → Access): send it as `X-Api-Key: <key>` on every `POST`. Without it, changes get `401`; `GET`s work either way |

Every `POST` needs either a body or a `Content-Length: 0` header. A request with neither is refused with
`400` before it reaches the engine. `curl` needs `-d` (`-d ""` for no body).

Port 8090 must be open in the media server's firewall. `install-engine.bat` (or `install.bat`) adds the rule
"Mosaic Engine Editor (8090)"; antivirus with its own firewall (ESET, for example) needs the same port allowed.
Anyone who can reach the port can change the walls, so keep the media server on the AV network.

## Concepts

- **Group**: one video wall or display, for example `main-wall` (16 displays), `2x2-wall`,
  `double-wide` or `lobby-display`. Groups are created in the editor (Outputs → *+ Group*).
- **Preset**: a saved picture for one group: which NDI streams it shows and where. A preset
  can be one stream filling the whole group, or several streams side by side or in quarters.
  Presets belong to their group; recalling one changes only that group.
- **NDI stream name**: the full name as NDI shows it, `MACHINE (Stream)`, for example
  `ZOWIEBOXPRO-54447 (ZowieBox-54447)`. It has to match exactly, including case and spaces.
  `GET /api/control` lists the streams the engine can see.
- **Scene** and **layout**: whole-canvas switches (all groups at once). Most control
  setups only need group presets.

**Ids** are lowercase forms of the names: spaces become `-`, other punctuation is dropped.
"Main wall" → `main-wall`, "Camera 1 full" → `camera-1-full`, "Camera + slides" → `camera-slides`.
The editor shows each preset's id under its name, and `GET /api/control` lists them all.
Saving a preset under a different name makes a new preset with a new id, so check the id
after creating presets.

**Transitions**: `transitionMs` is a crossfade time in milliseconds, from 0 (cut) to 10000.
The fade covers only the group being changed; other walls keep playing untouched. Starting
a new fade while one is running ends the running fade immediately.

Every change is saved on the media server and is still there after a restart.

**Canvases**: the media server can run several canvases at once (separate walls or groups of
receiving PCs, each its own layout). Requests are about the primary canvas, so control setups for a
single wall need nothing new. To control another canvas, add `X-Mosaic-Canvas: <canvas id>` to the
request (or `?canvas=<canvas id>`); `GET /api/canvases` lists them (`canvas-1`, `canvas-2`, ...) with
their names.

## Quick reference

| What | Request |
|---|---|
| Everything a control system needs: groups, presets, what's on, streams | `GET /api/control` |
| Recall a preset on a group | `POST /api/walls/{group}/presets/{preset}/recall` |
| Show one NDI stream on a whole group | `POST /api/walls/{group}/show` with `{"ndiName": "..."}` |
| Blank a group | `POST /api/walls/{group}/clear` |
| Restart the videos a group shows, together | `POST /api/walls/{group}/restart` |
| Recall a scene (all groups) | `POST /api/scenes/{scene}/recall` |
| Switch layout | `POST /api/layouts/{layout}/activate` |
| Is the engine up? | `GET /api/health` |

## Requests

### `GET /api/control`

One call that describes everything. Use it to fill the control system's button lists and to
show which preset is on.

```json
{
  "layout": { "id": "venue", "name": "Venue" },
  "groups": [
    {
      "id": "main-wall",
      "name": "Main wall",
      "displays": 16,
      "activePreset": "camera-1-full",
      "presets": [
        { "id": "camera-1-full", "name": "Camera 1 full" },
        { "id": "camera-slides", "name": "Camera + slides" }
      ],
      "showing": [
        { "name": "Camera 1", "type": "ndi", "ndiName": "ZOWIEBOXPRO-54447 (ZowieBox-54447)", "signal": "ok" }
      ],
      "noSignal": []
    },
    {
      "id": "double-wide",
      "name": "Double wide",
      "displays": 1,
      "activePreset": "",
      "presets": [],
      "showing": [],
      "noSignal": []
    }
  ],
  "endpoints": [
    { "id": "wall-pc-1", "name": "Wall PC 1", "online": true, "displays": 4, "displaysOk": 4 }
  ],
  "scenes": [ { "id": "opening", "name": "Opening" } ],
  "layouts": [ { "id": "venue", "name": "Venue", "active": true } ],
  "ndi": {
    "available": true,
    "streams": [ "ZOWIEBOXPRO-54447 (ZowieBox-54447)", "GFX-PC (Slides)" ]
  }
}
```

- `activePreset` is the last preset recalled or saved on that group. It becomes `""` after a
  `show` or `clear` call. It doesn't change when someone edits the group by hand in the editor.
- `showing` lists the group's current layers, bottom first. An empty list means the group shows
  nothing of its own, only the canvas background under it.
- `endpoints` are the receiving PCs set up in the editor: whether each is online, and how many of
  its displays play the stream they should (`displaysOk` of `displays`). Use it for a status page.
- `noSignal` names the group's streams that have sent nothing for 2 s. The group shows its fallback
  image there if one is set (editor: group → Content → *If a stream drops, show*).
- `ndi.streams` are the streams NDI discovery currently sees on the network.

### `POST /api/walls/{group}/presets/{preset}/recall`

Shows a saved preset on one group. The body is optional:

```json
{ "transitionMs": 500 }
```

Reply:

```json
{ "group": "main-wall", "recalled": "camera-1-full", "transitionMs": 500 }
```

`404` with `{"error": "no such wall group"}` or `{"error": "no such preset in group 'main-wall'"}`
if an id is wrong.

When the new preset uses a stream the group was already showing, that stream doesn't
reconnect, so it switches instantly. A stream that wasn't on screen needs a moment to connect
(typically under a second). Until its first frame arrives, the area shows near-black grey.

### `POST /api/walls/{group}/show`

Fills a whole group with one NDI stream, without needing a preset. This is handy for a
"what's on the double-wide" source selector.

```json
{ "ndiName": "GFX-PC (Slides)", "transitionMs": 500 }
```

Reply:

```json
{ "group": "double-wide", "showing": "GFX-PC (Slides)", "transitionMs": 500 }
```

`ndiName` is required (`400` without it). The name isn't checked against discovery: if
nothing on the network has that name, the group shows near-black grey ("no signal") until it appears.

### `POST /api/walls/{group}/clear`

Removes what the group shows. The group then shows whatever lies under it on the canvas: the
background colour (black by default), or a canvas-wide source if the layout has one. The body is
optional: `{"transitionMs": 500}`.

```json
{ "group": "lobby-display", "cleared": true, "transitionMs": 500 }
```

### `POST /api/walls/{group}/restart`

Starts every video file the group shows from its first frame, at the same moment, for example at the
start of a show segment. Reply: `{"group": "main-wall", "restarted": ["main-wall--video"]}`. To pause and resume
one video, `PATCH /api/sources/{source}` with `{"playing": false}` / `{"playing": true}`. The source id is
listed by `GET /api/sources`.

### `POST /api/scenes/{scene}/recall`

Replaces everything on the canvas, all groups at once, with a scene saved in the editor
(Scenes → *+ Save*). Body optional: `{"transitionMs": 1000}`. This fade covers the whole canvas.

### `POST /api/layouts/{layout}/activate`

Switches to another saved layout. A layout includes the displays themselves, so use this for
a different room setup, not for changing content. Body optional: `{"transitionMs": 1000}`
(the fade only happens when both layouts have the same canvas size).

### `GET /api/health`

`{"ok": true}` while the engine is running. Poll it every few seconds to show an
"engine online" indicator.

### `GET /api/audio`

The media server's sound outputs (set up in Settings → Audio; stored as the layout's
`audioOutputs`): `wallDelayMs`, the PC's sound `devices`, discovered `ndiSources`, and per output
its `levelDb` (left/right peak since the last read), `delayMs`, and `error` / `sourceError`. To change
an output's volume from a control system, `PUT /api/layout` with the whole `audioOutputs` list.

### `GET /api/system/update` · `POST /api/system/update`

Versions (git commits) of the media server and each receiving PC, and the last update's outcome.
`POST {"target": "all"}` updates every receiving PC (at its next check-in) and then the media server,
which stops for a minute or two while it rebuilds; `"receivers"` skips the media server. This is what
Settings → Updates in the editor uses; a control system normally doesn't need it.

## Making presets (setup, not day-to-day control)

Presets are normally made in the editor:

1. Open `http://<media-server>:8090`. Click a group's name in the output list, then open the **Content** tab.
2. Choose **Show an NDI stream on the whole group**, or **Add a stream as another layer** for split layouts.
   To place a layer, click it under *Showing now*, then use *Left ½*, *Top-right ¼* and so on.
   Dragging it in **Edit sources** works too.
3. Click **+ Save current as preset** and give it a name. Its id appears under the name.

The same through the API:

| What | Request |
|---|---|
| List a group's presets, with their full content | `GET /api/walls/{group}/presets` |
| Save what the group shows now as a preset (same name overwrites) | `POST /api/walls/{group}/presets` with `{"name": "Camera 1 full"}` |
| Delete a preset | `DELETE /api/walls/{group}/presets/{preset}` |
| List groups with their settings | `GET /api/walls` |

The full engine API (outputs, sources, layouts) is in the [README](../README.md#rest-api).

## Examples

The examples use media server `192.168.1.187`, group `main-wall` and preset `camera-1-full`.

**curl in Windows Command Prompt.** CMD needs the inner quotes escaped:

With an API key set, add `-H "X-Api-Key: <key>"` to each command (and `-Headers @{"X-Api-Key"="<key>"}` in PowerShell).

```bat
curl -X POST http://192.168.1.187:8090/api/walls/main-wall/presets/camera-1-full/recall -H "Content-Type: application/json" -d "{\"transitionMs\":500}"
curl -X POST http://192.168.1.187:8090/api/walls/double-wide/show -H "Content-Type: application/json" -d "{\"ndiName\":\"GFX-PC (Slides)\",\"transitionMs\":500}"
curl http://192.168.1.187:8090/api/control
```

**curl on macOS / Linux:**

```bash
curl -X POST http://192.168.1.187:8090/api/walls/main-wall/presets/camera-1-full/recall \
     -H 'Content-Type: application/json' -d '{"transitionMs":500}'
```

**PowerShell:**

```powershell
Invoke-RestMethod -Method Post -Uri http://192.168.1.187:8090/api/walls/main-wall/presets/camera-1-full/recall `
  -ContentType application/json -Body '{"transitionMs":500}'
(Invoke-RestMethod http://192.168.1.187:8090/api/control).groups | Format-Table id, activePreset
```

**Bitfocus Companion** ("Generic: HTTP Requests" connection):

- Base URL: `http://192.168.1.187:8090`
- Button action: **POST**, URL `/api/walls/main-wall/presets/camera-1-full/recall`,
  body `{"transitionMs":500}`, header `Content-Type: application/json`
- One button per preset. For a stream picker, use `/api/walls/double-wide/show` with
  `{"ndiName":"GFX-PC (Slides)"}`.

**Crestron / Q-SYS / AMX** (any TCP client that can send raw HTTP). Open a TCP connection to
port 8090 and send this, with CRLF line endings and an empty line before the body:

```http
POST /api/walls/main-wall/presets/camera-1-full/recall HTTP/1.1
Host: 192.168.1.187:8090
Content-Type: application/json
Content-Length: 20
X-Api-Key: <key, only if one is set>
Connection: close

{"transitionMs":500}
```

`Content-Length` must be the exact byte count of the body. To send no body, use
`Content-Length: 0`, and the preset is recalled with a cut. Look for `HTTP/1.1 200` in the reply.
Q-SYS Lua can use `HttpClient.Upload{Url=..., Method="POST", Data=..., Headers={["Content-Type"]="application/json"}}`.

## Live status

`GET /api/events` is a Server-Sent Events stream. About twice a second it sends
`data: {"metrics": ..., "layoutVersion": N}`. When `layoutVersion` changes, something changed:
call `GET /api/control` again to refresh which presets are on. Use this if the control panel
should follow changes made from the editor or another controller.
